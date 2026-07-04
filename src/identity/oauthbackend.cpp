// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "oauthbackend.h"

#include "constanttime.h"
#include "edgereplyhandler.h"
#include "jwksverifier.h"
#include "secrets.h"

#include "proxypolicy.h"

#include <QAbstractOAuth>
#include <QAbstractOAuth2>
#include <QDateTime>
#include "tracescope.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QSet>
#include <QOAuth2AuthorizationCodeFlow>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>

#include <utility>

namespace SynQt {

namespace {

// The name of the first endpoint of this provider that may not be spoken to over the
// network as configured, or empty when every one of them is safe.
QString insecureEndpoint(const IdentityProviderConfig &provider)
{
    const std::pair<const char *, QUrl> endpoints[]{
        {"authorization", provider.authorizeUrl},
        {"token", provider.tokenUrl},
        {"userinfo", provider.userinfoUrl},
        {"emails", provider.emailsUrl},
        {"JWKS", provider.jwksUrl}};
    for (const auto &[name, url] : endpoints) {
        if (!url.isEmpty() && !isSecureIdentityEndpoint(url)) {
            return QString::fromUtf8(name);
        }
    }
    return QString{};
}

// How many logins may be in flight at once, waiting for a browser to come back from the
// provider. Each one holds a QOAuth2AuthorizationCodeFlow for the five minutes a login is
// given, and the route that creates them is open to anybody who can reach the edge, so
// without a ceiling a stream of GETs to /auth/login is a way to make the edge allocate
// until it stops. Far above any real concurrency. A thousand people signing in within the
// same five minutes is a busy day, not an attack.
constexpr int kMaxPendingLogins{1024};

// How large an answer from a provider endpoint may be. A token response and a profile are
// both a few hundred bytes. The ceiling exists because QNetworkReply buffers a whole body
// before anybody reads it, so without one the size of a login's memory cost is decided by
// whatever answered. Generous enough that no provider approaches it.
constexpr qint64 kMaxProviderResponseBytes{1024 * 1024};

// How long one request to a provider may take, whichever endpoint it is.
constexpr int kProviderTimeoutMs{15000};

// How often to look for tokens nobody claimed, given how long they may go unclaimed:
// twice a window, and never more than once a minute nor less often than that.
int unclaimedSweepMs(int windowSeconds)
{
    return qBound(1000, (windowSeconds * 1000) / 2, 60000);
}

// Refuse an answer past kMaxProviderResponseBytes while it is still arriving, and walk out
// on one that takes longer than kProviderTimeoutMs. Both mark the reply before aborting
// it, because an aborted reply reports only that it was cancelled and the handler has to
// tell a deadline from a refusal. Connected to the reply, so they go when it does.
void boundReply(QNetworkReply *reply)
{
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [reply](qint64 received, qint64 total) {
        if (received > kMaxProviderResponseBytes || total > kMaxProviderResponseBytes) {
            reply->setProperty("synqtTooLarge", true);
            reply->abort();
        }
    });
    QTimer *deadline{new QTimer{reply}};
    deadline->setSingleShot(true);
    QObject::connect(deadline, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("synqtTimedOut", true);
        reply->abort();
    });
    deadline->start(kProviderTimeoutMs);
}

// A reply that was walked out on, or refused for its size, read the one way it can be:
// off the marks boundReply left. Empty when the reply finished on its own terms.
QString boundReplyFailure(const QNetworkReply *reply, const QUrl &url)
{
    if (reply->property("synqtTimedOut").toBool()) {
        return QStringLiteral("the request to %1 timed out")
            .arg(url.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery));
    }
    if (reply->property("synqtTooLarge").toBool()) {
        return QStringLiteral("the answer from %1 is larger than a provider's can be")
            .arg(url.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery));
    }
    return QString{};
}

} // namespace

OAuthBackend::OAuthBackend(IdentityConfig config, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
{
    // Unconditional, unlike the refresh sweep. Whether tokens are refreshed is a project's
    // choice, and whether a secret nobody claimed is let go of is not.
    m_unclaimedTimer = new QTimer{this};
    connect(m_unclaimedTimer, &QTimer::timeout, this, [this]() { releaseUnclaimed(); });
    m_unclaimedTimer->start(unclaimedSweepMs(m_unclaimedWindowSeconds));
}

void OAuthBackend::setUnclaimedWindow(int seconds)
{
    m_unclaimedWindowSeconds = qMax(0, seconds);
    // At least twice per window, so an entry is never kept for much longer than the
    // window says, and never more often than once a minute on the default.
    m_unclaimedTimer->start(unclaimedSweepMs(m_unclaimedWindowSeconds));
}

void OAuthBackend::releaseUnclaimed()
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    const qint64 window{static_cast<qint64>(m_unclaimedWindowSeconds) * 1000};
    for (auto it{m_tokens.begin()}; it != m_tokens.end();) {
        if (!it->bound && (now - it->storedMs) >= window) {
            // Said out loud. A login that got as far as the provider and then had nobody
            // to hand the session to needs reporting, and the alternative to
            // saying so is a count that quietly goes down.
            qWarning("SynQt: letting go of the tokens of a login no session was bound to "
                     "within %d seconds; the caller that started it did not come back",
                     m_unclaimedWindowSeconds);
            it = m_tokens.erase(it);
        } else {
            ++it;
        }
    }
}

OAuthBackend::~OAuthBackend() = default;

QNetworkAccessManager *OAuthBackend::network()
{
    // One manager for every call this backend makes, created on the first of them and
    // given the egress route a server takes. What its own environment names, never the
    // machine's browser settings (see SynQt::applyEnvironmentProxy).
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
        applyEnvironmentProxy(m_network);
    }
    return m_network;
}

bool OAuthBackend::providerExists(const QString &name) const
{
    return m_config.provider(name) != nullptr;
}

bool OAuthBackend::isDevStub(const QString &name) const
{
    const IdentityProviderConfig *provider{m_config.provider(name)};
    return provider && provider->devStub;
}

QOAuth2AuthorizationCodeFlow *OAuthBackend::makeFlow(const IdentityProviderConfig &provider,
                                                    const QString &redirectUri)
{
    QOAuth2AuthorizationCodeFlow *flow{new QOAuth2AuthorizationCodeFlow{
        provider.clientId, provider.authorizeUrl, provider.tokenUrl, network(), this}};
    // The client secret is held here and sent only in the server-side token exchange.
    flow->setClientIdentifierSharedKey(provider.clientSecret);
    flow->setPkceMethod(QOAuth2AuthorizationCodeFlow::PkceMethod::S256);
    if (!provider.scopes.isEmpty()) {
        // requestedScopeTokens, not the space-joined scope string: setScope is deprecated
        // since 6.11 and Qt joins the tokens itself for the request.
        QSet<QByteArray> tokens;
        tokens.reserve(provider.scopes.size());
        for (const QString &scope : provider.scopes) {
            tokens.insert(scope.toUtf8());
        }
        flow->setRequestedScopeTokens(tokens);
    }
    // The redirect_uri is the caller's public callback route, not a loopback port.
    flow->setReplyHandler(new EdgeReplyHandler{redirectUri, flow});
    return flow;
}

OAuthBackend::BeginResult OAuthBackend::begin(const QString &providerName,
                                              const QString &redirectUri,
                                              const QString &binding,
                                              const QString &context)
{
    expirePending();
    BeginResult result;
    const IdentityProviderConfig *provider{m_config.provider(providerName)};
    if (!provider) {
        result.error = QStringLiteral("unknown provider");
        return result;
    }
    if (provider->devStub && !m_config.allowDevStub) {
        result.error = QStringLiteral("dev stub provider is disabled");
        return result;
    }
    // Refused here, at the last moment before a browser is sent anywhere, because this is
    // the one place every login must pass through however the config was assembled.
    if (const QString endpoint{insecureEndpoint(*provider)}; !endpoint.isEmpty()) {
        qWarning("SynQt: identity provider '%s' has a plaintext %s endpoint; refusing the "
                 "login rather than sending the secret, the code or the signing keys over "
                 "http", qUtf8Printable(providerName), qUtf8Printable(endpoint));
        result.error = QStringLiteral("insecure provider endpoint");
        return result;
    }
    // An ID token is checked against the issuer the provider was configured with, and a
    // provider that names none skips that check entirely. Refused here rather than left to
    // pass silently, because "the iss claim is not compared" is not a thing anybody chooses
    // on purpose, and the same place already refuses the other configuration that would
    // send a login somewhere it should not go.
    if (provider->useIdToken && provider->issuer.isEmpty()) {
        qWarning("SynQt: identity provider '%s' verifies ID tokens but names no issuer, so "
                 "the iss claim would not be checked at all; set identity.providers.%s."
                 "issuer", qUtf8Printable(providerName), qUtf8Printable(providerName));
        result.error = QStringLiteral("provider names no issuer");
        return result;
    }
    // Bounded before the flow is built, so a refused login costs one comparison rather than
    // an object held for five minutes. Sweeping first (expirePending, above) means this is
    // reached only when that many logins are in flight.
    if (m_pending.size() >= kMaxPendingLogins) {
        qWarning("SynQt: %d logins are already in flight and none has completed; refusing "
                 "this one rather than growing further", kMaxPendingLogins);
        result.error = QStringLiteral("too many logins in flight");
        return result;
    }

    QOAuth2AuthorizationCodeFlow *flow{makeFlow(*provider, redirectUri)};
    const QString state{randomToken()};
    flow->setState(state);

    // OpenID Connect: bind the ID token to this request with a nonce carried in the
    // authorization request and checked on the returned ID token.
    //
    // Through Qt's own nonce rather than by adding a parameter, and the difference is not
    // cosmetic. `NonceMode::Automatic` is the default, and it puts a nonce of Qt's own in
    // the request whenever the scope contains `openid`. A second one inserted here went
    // into the same QMultiMap, so the authorization request carried the parameter twice,
    // with two different values. RFC 6749 section 3.1 says a request parameter MUST NOT
    // be included more than once, and a provider that enforces it answers invalid_request
    // rather than signing anybody in. One that does not enforce it picks whichever value
    // it reads first, which is a coin toss on whether the ID token's nonce then matches
    // the one recorded here. Setting the mode explicitly and handing Qt the value keeps
    // this framework's own random token and leaves exactly one nonce in the request.
    QString nonce;
    if (provider->useIdToken) {
        nonce = randomToken();
        flow->setNonceMode(QAbstractOAuth2::NonceMode::Enabled);
        flow->setNonce(nonce);
    }

    QUrl authorizeUrl;
    connect(flow, &QOAuth2AuthorizationCodeFlow::authorizeWithBrowser, flow,
            [&authorizeUrl](const QUrl &url) { authorizeUrl = url; });
    flow->grant();  // builds the auth URL (PKCE + state) and emits authorizeWithBrowser

    if (authorizeUrl.isEmpty()) {
        flow->deleteLater();
        result.error = QStringLiteral("could not build the authorization URL");
        return result;
    }

    Pending pending;
    pending.flow = flow;
    pending.providerName = providerName;
    pending.nonce = nonce;
    pending.binding = binding;
    pending.context = context;
    pending.createdMs = QDateTime::currentMSecsSinceEpoch();
    m_pending.insert(state, pending);

    result.state = state;
    result.authorizeUrl = authorizeUrl;
    return result;
}

/// The steps of one exchange, taken as each reply arrives.
///
/// Owned by the backend and gone once it has answered. The flow it drives is the pending
/// login's, and the tokens it ends with are the backend's. Every step reports through the
/// one `done` the caller handed in, exactly once.
class OAuthBackend::ExchangeJob : public QObject
{
public:
    ExchangeJob(OAuthBackend *backend, Pending pending, QString state,
                IdentityProviderConfig provider, ExchangeCallback done)
        : QObject{backend}
        , m_backend{backend}
        , m_pending{std::move(pending)}
        , m_state{std::move(state)}
        , m_provider{std::move(provider)}
        , m_done{std::move(done)}
    {
    }

    void start(const QString &code)
    {
        QOAuth2AuthorizationCodeFlow *flow{m_pending.flow};
        connect(flow, &QOAuth2AuthorizationCodeFlow::granted, this,
                &ExchangeJob::resolveIdentity);
        connect(flow, &QAbstractOAuth::requestFailed, this,
                [this](QAbstractOAuth::Error) {
            fail(QStringLiteral("token exchange failed"));
        });
        QTimer *deadline{new QTimer{this}};
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, this, [this]() {
            fail(QStringLiteral("token exchange failed"));
        });
        deadline->start(kProviderTimeoutMs);

        auto *handler{qobject_cast<EdgeReplyHandler *>(flow->replyHandler())};
        handler->receiveCallback(QVariantMap{{QStringLiteral("code"), code},
                                             {QStringLiteral("state"), m_state}});
    }

private:
    void resolveIdentity()
    {
        QOAuth2AuthorizationCodeFlow *flow{m_pending.flow};
        if (m_provider.useIdToken) {
            // OpenID Connect: identity from the ID token, whose signature is verified
            // against the provider JWKS before any claim is trusted.
            if (!m_backend->m_jwks) {
                m_backend->m_jwks = new JwksVerifier{m_backend->network(), m_backend};
            }
            const QPointer<ExchangeJob> self{this};
            m_backend->m_jwks->verifyAsync(flow->idToken(), m_provider, m_pending.nonce,
                                           [self](const QVariantMap &claims,
                                                  const QString &error) {
                if (!self) {
                    return;
                }
                if (claims.isEmpty()) {
                    self->fail(error);
                    return;
                }
                QVariantMap identity;
                identity.insert(QStringLiteral("sub"),
                                claims.value(QStringLiteral("sub")).toString());
                identity.insert(QStringLiteral("login"),
                                claims.value(QStringLiteral("preferred_username")));
                identity.insert(QStringLiteral("name"), claims.value(QStringLiteral("name")));
                const QString email{claims.value(QStringLiteral("email")).toString()};
                identity.insert(QStringLiteral("email"),
                                email.isEmpty() ? QVariant{} : QVariant{email});
                self->finish(identity);
            });
            return;
        }

        if (m_provider.userinfoUrl.isEmpty()) {
            fail(QStringLiteral("provider has no userinfo endpoint"));
            return;
        }
        m_backend->httpGet(m_provider.userinfoUrl, flow->token(), this,
                           [this](const QByteArray &body, const QString &error) {
            onUserinfo(body, error);
        });
    }

    void onUserinfo(const QByteArray &body, const QString &error)
    {
        const QJsonDocument document{QJsonDocument::fromJson(body)};
        if (!document.isObject()) {
            fail(error.isEmpty() ? QStringLiteral("userinfo response was not an object")
                                 : error);
            return;
        }
        const QVariantMap profile{document.object().toVariantMap()};

        // The subject, first and required. Everything downstream keys on it: the scope
        // mapping reads it, a device credential is enrolled against it, and an application
        // tells one user from another by it. A profile that carries none (a misspelled
        // `sub_field`, a provider that answered something else) would otherwise sign every
        // such visitor in as the same empty subject, which is one shared account rather
        // than a failed login.
        const QString subject{profile.value(m_provider.subField).toString()};
        if (subject.isEmpty()) {
            fail(QStringLiteral("userinfo response carried no '%1'").arg(m_provider.subField));
            return;
        }

        m_identity.insert(QStringLiteral("sub"), subject);
        m_identity.insert(QStringLiteral("login"), profile.value(m_provider.loginField));
        m_identity.insert(QStringLiteral("name"), profile.value(m_provider.nameField));
        const QVariant email{profile.value(m_provider.emailField)};
        if ((email.isNull() || email.toString().isEmpty()) && !m_provider.emailsUrl.isEmpty()) {
            // GitHub-style fallback. The primary verified address from the emails endpoint.
            m_backend->httpGet(m_provider.emailsUrl, m_pending.flow->token(), this,
                               [this](const QByteArray &emailsBody, const QString &) {
                onEmails(emailsBody);
            });
            return;
        }
        finishWithEmail(email.toString());
    }

    void onEmails(const QByteArray &body)
    {
        QString email;
        const QJsonDocument emailsDoc{QJsonDocument::fromJson(body)};
        if (emailsDoc.isArray()) {
            // Copy initialized, not braced: QJsonArray's initializer_list constructor
            // would take this array as a single element (see Topology::topologyFromJson).
            const QJsonArray emails = emailsDoc.array();
            for (const QJsonValue &value : emails) {
                const QJsonObject entry{value.toObject()};
                if (entry.value(QStringLiteral("primary")).toBool()
                    && entry.value(QStringLiteral("verified")).toBool()) {
                    email = entry.value(QStringLiteral("email")).toString();
                    break;
                }
            }
        }
        finishWithEmail(email);
    }

    void finishWithEmail(const QString &email)
    {
        // Email is nullable. A valid address or a null QVariant, never an empty string.
        m_identity.insert(QStringLiteral("email"),
                          email.isEmpty() ? QVariant{} : QVariant{email});
        finish(m_identity);
    }

    void finish(const QVariantMap &identity)
    {
        if (m_answered) {
            return;
        }
        m_answered = true;
        QOAuth2AuthorizationCodeFlow *flow{m_pending.flow};
        // Store the tokens under the state key (rekeyed to the session id once it exists).
        // The tokens never leave this engine and are never logged.
        TokenEntry entry;
        entry.providerName = m_pending.providerName;
        entry.accessToken = flow->token();
        entry.refreshToken = flow->refreshToken();
        entry.idToken = flow->idToken();
        const QDateTime expiry{flow->expirationAt()};
        entry.expiresAtMs = expiry.isValid() ? expiry.toMSecsSinceEpoch() : 0;
        // Under the state key and unclaimed. The caller binds a session to it next, and
        // OAuthBackend::releaseUnclaimed is what happens when it never does.
        entry.storedMs = QDateTime::currentMSecsSinceEpoch();
        entry.bound = false;
        m_backend->m_tokens.insert(m_state, entry);
        flow->deleteLater();

        ExchangeResult result;
        result.identity = identity;
        result.tokenKey = m_state;
        result.context = m_pending.context;
        answer(result);
    }

    void fail(const QString &error)
    {
        if (m_answered) {
            return;
        }
        m_answered = true;
        m_pending.flow->deleteLater();
        ExchangeResult result;
        result.error = error.isEmpty() ? QStringLiteral("identity could not be resolved")
                                       : error;
        result.context = m_pending.context;
        answer(result);
    }

    void answer(const ExchangeResult &result)
    {
        // Retired before the caller is told, so a caller that starts another exchange
        // from inside `done` finds this one gone. The callback is moved out first: the
        // deferred delete cannot run under this stack, but a callback that owns something
        // should not have to know that.
        const ExchangeCallback done{std::move(m_done)};
        deleteLater();
        done(result);
    }

    OAuthBackend *m_backend;
    Pending m_pending;
    QString m_state;
    IdentityProviderConfig m_provider;
    ExchangeCallback m_done;
    QVariantMap m_identity;
    bool m_answered{false};
};

OAuthBackend::ExchangeResult OAuthBackend::exchange(const QString &state, const QString &code,
                                                    const QString &redirectUri,
                                                    const QString &presentedBinding)
{
    // The asynchronous form, waited on. A route handler may wait (it answers when it
    // returns, and the identity routes bound how many of them may be waiting at once). A
    // slot may not, and takes exchangeAsync directly.
    ExchangeResult result;
    bool answered{false};
    QEventLoop loop;
    exchangeAsync(state, code, redirectUri, presentedBinding,
                  [&result, &answered, &loop](const ExchangeResult &outcome) {
        result = outcome;
        answered = true;
        loop.quit();
    });
    if (!answered) {
        // The loop serves other callers while it spins, and they are not this caller's
        // story. See SynQt::TraceScope on detaching.
        const SynQt::TraceScope untraced{SynQt::TraceContext{}};
        loop.exec();
    }
    return result;
}

void OAuthBackend::exchangeAsync(const QString &state, const QString &code,
                                 const QString &redirectUri, const QString &presentedBinding,
                                 ExchangeCallback done)
{
    Q_UNUSED(redirectUri);  // the pending flow already carries the matching redirect_uri

    // Only a state this engine issued (and still holds) is accepted. An unknown or replayed
    // state is rejected before any token exchange.
    if (state.isEmpty() || !m_pending.contains(state)) {
        ExchangeResult result;
        result.error = QStringLiteral("invalid or expired state");
        done(result);
        return;
    }
    Pending pending{m_pending.take(state)};
    QOAuth2AuthorizationCodeFlow *flow{pending.flow};

    // Login-CSRF defense, checked here rather than by the caller and checked before the
    // code is spent. Here, because the record it is checked against lives here, and a
    // check that travels away from its data is a check each caller can forget. Before,
    // because exchanging first would hand a real authorization code to whoever sent this
    // callback and only then notice it was not the browser that started the login.
    //
    // The pending record is already taken, so this is single-use whichever way it goes: a
    // callback replayed after a success finds no state, and one replayed after a mismatch
    // finds none either.
    if (!pending.binding.isEmpty() && !constantTimeEquals(presentedBinding, pending.binding)) {
        flow->deleteLater();
        ExchangeResult result;
        result.error = QStringLiteral("login session mismatch");
        done(result);
        return;
    }

    const IdentityProviderConfig *provider{m_config.provider(pending.providerName)};
    if (!provider) {
        flow->deleteLater();
        ExchangeResult result;
        result.error = QStringLiteral("unknown provider");
        result.context = pending.context;
        done(result);
        return;
    }

    // From here on nothing is waited for. The job answers through `done` as the provider
    // does, and takes itself down when it has.
    auto *job{new ExchangeJob{this, std::move(pending), state, *provider, std::move(done)}};
    job->start(code);
}

void OAuthBackend::rekeyTokens(const QString &fromKey, const QString &toKey)
{
    if (fromKey == toKey) {
        return;
    }
    const auto it{m_tokens.constFind(fromKey)};
    if (it == m_tokens.constEnd()) {
        return;
    }
    TokenEntry moved{it.value()};
    // Claimed: a session names it now, so it lives and dies with that session rather than
    // with the window an unbound login gets.
    moved.bound = true;
    m_tokens.insert(toKey, moved);
    m_tokens.erase(m_tokens.find(fromKey));
}

QVariantMap OAuthBackend::tokens(const QString &key) const
{
    const auto it{m_tokens.constFind(key)};
    if (it == m_tokens.constEnd()) {
        return {};
    }
    QVariantMap out;
    out.insert(QStringLiteral("access_token"), it->accessToken);
    out.insert(QStringLiteral("refresh_token"), it->refreshToken);
    out.insert(QStringLiteral("id_token"), it->idToken);
    if (it->expiresAtMs > 0) {
        out.insert(QStringLiteral("expires_at"), static_cast<double>(it->expiresAtMs));
    }
    return out;
}

void OAuthBackend::releaseTokens(const QString &key)
{
    m_tokens.remove(key);
}

int OAuthBackend::heldTokenCount() const
{
    return static_cast<int>(m_tokens.size());
}

void OAuthBackend::setAutoRefresh(int intervalSeconds, int marginSeconds)
{
    m_refreshMargin = marginSeconds;
    if (intervalSeconds <= 0) {
        if (m_refreshTimer) {
            m_refreshTimer->stop();
        }
        return;
    }
    if (!m_refreshTimer) {
        m_refreshTimer = new QTimer{this};
        connect(m_refreshTimer, &QTimer::timeout, this,
                [this]() { refreshExpiring(m_refreshMargin); });
    }
    m_refreshTimer->start(intervalSeconds * 1000);
}

int OAuthBackend::refreshExpiring(int marginSeconds)
{
    // One sweep at a time. Each refresh waits on the provider in a nested event loop, and
    // the refresh timer keeps firing while it does, so without this a slow provider starts
    // a second sweep over the same due list inside the first. The same refresh token spent
    // twice, which a provider that rotates them answers by invalidating both.
    if (m_sweeping) {
        return 0;
    }
    m_sweeping = true;
    const auto done{qScopeGuard([this]() { m_sweeping = false; })};

    const qint64 threshold{QDateTime::currentMSecsSinceEpoch()
                           + static_cast<qint64>(marginSeconds) * 1000};
    int refreshed{0};
    // Collect first. refreshOne mutates m_tokens, so do not iterate it while refreshing.
    QStringList due;
    for (auto it{m_tokens.constBegin()}; it != m_tokens.constEnd(); ++it) {
        if (it->refreshToken.isEmpty() || it->expiresAtMs <= 0) {
            continue;
        }
        if (it->expiresAtMs <= threshold) {
            due.append(it.key());
        }
    }
    for (const QString &key : due) {
        if (refreshOne(key)) {
            ++refreshed;
            emit tokensRefreshed(key);
        }
    }
    return refreshed;
}

bool OAuthBackend::refreshOne(const QString &key)
{
    // Read out by value, never held as an iterator. Everything below waits on the network
    // in a nested event loop, and that loop runs every other handler this backend has: a
    // callback completing inserts into m_tokens and can rehash it, a session expiring or
    // being revoked erases from it. Either one leaves an iterator taken before the wait
    // pointing at memory the hash no longer owns, and the writes at the end of this
    // function land there. So the entry is copied out, the wait happens, and the row is
    // looked up again afterwards under the same key.
    const auto before{m_tokens.constFind(key)};
    if (before == m_tokens.constEnd() || before->refreshToken.isEmpty()) {
        return false;
    }
    const TokenEntry entry{before.value()};
    const IdentityProviderConfig *provider{m_config.provider(entry.providerName)};
    if (!provider) {
        return false;
    }

    // RFC 6749 section 6: exchange the refresh token for a fresh access token, server-side. The
    // client secret stays here. The browser is never involved.
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    body.addQueryItem(QStringLiteral("refresh_token"), entry.refreshToken);
    body.addQueryItem(QStringLiteral("client_id"), provider->clientId);
    if (!provider->clientSecret.isEmpty()) {
        body.addQueryItem(QStringLiteral("client_secret"), provider->clientSecret);
    }
    if (!provider->scopes.isEmpty()) {
        body.addQueryItem(QStringLiteral("scope"), provider->scopes.join(QLatin1Char(' ')));
    }

    QNetworkRequest request{provider->tokenUrl};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QByteArrayLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    QNetworkReply *reply{
        network()->post(request, body.toString(QUrl::FullyEncoded).toUtf8())};

    // A sweep runs from a timer and not from a slot or a route, so it may wait. What it
    // waits with is the same bound every other request to a provider has. A reply the
    // deadline walked out on is finished with an abort, which reads below as an error and
    // keeps the old entry, which is the right outcome either way. It may still have time
    // left on it.
    boundReply(reply);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    {
        // The loop serves other callers while it spins, and they are not this caller's
        // story. See SynQt::TraceScope on detaching.
        const SynQt::TraceScope untraced{SynQt::TraceContext{}};
        loop.exec();
    }
    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return false;
    }
    const QJsonDocument document{QJsonDocument::fromJson(reply->readAll())};
    reply->deleteLater();
    if (!document.isObject()) {
        return false;
    }
    const QJsonObject object{document.object()};
    const QString access{object.value(QStringLiteral("access_token")).toString()};
    if (access.isEmpty()) {
        return false;  // a provider error (e.g. invalid_grant): keep the old entry
    }

    // Looked up again rather than written through the iterator taken at the top: the wait
    // above ran every other handler, and the row may have been rekeyed to a new session
    // id, replaced by a second sign-in, or erased by a revocation while it did. A row that
    // is no longer there is a session that ended mid-refresh, and the fresh tokens are
    // dropped. Writing them back would resurrect a credential somebody revoked.
    const auto after{m_tokens.find(key)};
    if (after == m_tokens.end()) {
        return false;
    }
    after->accessToken = access;
    // A provider may rotate the refresh token. Keep the old one if it does not.
    const QString rotated{object.value(QStringLiteral("refresh_token")).toString()};
    if (!rotated.isEmpty()) {
        after->refreshToken = rotated;
    }
    const QString freshId{object.value(QStringLiteral("id_token")).toString()};
    if (!freshId.isEmpty()) {
        after->idToken = freshId;
    }
    // A lifetime the provider named, or none at all, and never the one that expired.
    //
    // `expires_in` is RECOMMENDED and not REQUIRED (RFC 6749 section 5.1), so a provider may
    // conform and leave it out. Keeping the old value then leaves the entry permanently past
    // its threshold. The sweep picks it up again on its next pass, refreshes it again, gets
    // no lifetime again, and spends a refresh token against the provider once per interval
    // for the life of the session. Zero is what the exchange already writes for a token
    // whose expiry the provider did not give (see exchange()), and refreshExpiring() skips
    // an entry at zero, because a deadline nothing knows is not one a timer can act on.
    const QJsonValue expiresIn{object.value(QStringLiteral("expires_in"))};
    after->expiresAtMs = expiresIn.isDouble()
        ? QDateTime::currentMSecsSinceEpoch()
              + static_cast<qint64>(expiresIn.toDouble()) * 1000
        : 0;
    return true;
}

void OAuthBackend::httpGet(const QUrl &url, const QString &bearer, QObject *context,
                           BodyCallback done)
{
    QNetworkRequest request{url};
    request.setRawHeader(QByteArrayLiteral("Authorization"), "Bearer " + bearer.toUtf8());
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    QNetworkReply *reply{network()->get(request)};
    boundReply(reply);
    // Answered on `context`, so a job that is gone by the time the provider answers is
    // not told anything. The reply is still finished and freed, on its own.
    connect(reply, &QNetworkReply::finished, context, [reply, url, done]() {
        reply->deleteLater();
        const QString bound{boundReplyFailure(reply, url)};
        if (!bound.isEmpty()) {
            done({}, bound);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            done({}, reply->errorString());
            return;
        }
        done(reply->readAll(), QString{});
    });
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void OAuthBackend::expirePending()
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    for (auto it{m_pending.begin()}; it != m_pending.end();) {
        if (now - it->createdMs > 5 * 60 * 1000) {  // a login has 5 minutes to complete
            it->flow->deleteLater();
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace SynQt
