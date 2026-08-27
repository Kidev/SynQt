// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "oauthbackend.h"

#include "boundedreply.h"
#include "constanttime.h"
#include "edgereplyhandler.h"
#include "jwksverifier.h"
#include "proxypolicy.h"
#include "secrets.h"
#include "tracescope.h"

#include <QAbstractOAuth>
#include <QAbstractOAuth2>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOAuth2AuthorizationCodeFlow>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>

#include <chrono>
#include <utility>

namespace SynQt {

namespace {

/// Whether a provider's `email_verified` (or Google's older `verified_email`) says yes. A
/// JSON boolean, or the text "true" some providers send instead. Anything else, absence
/// included, is no.
bool saysVerified(const QVariant &value)
{
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool();
    }
    return value.toString().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
}

/// Whether a userinfo profile states that its address is unverified. A profile that says
/// nothing is taken as it stands (GitHub's `/user` carries only an address it verified).
bool statesUnverified(const QVariantMap &profile)
{
    for (const QString &key : {QStringLiteral("email_verified"),
                               QStringLiteral("verified_email")}) {
        if (profile.contains(key) && !saysVerified(profile.value(key))) {
            return true;
        }
    }
    return false;
}

// The name of the first provider endpoint that may not be reached as configured, or empty
// when all are safe.
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

// The maximum size of a provider response. Token responses and profiles are a few hundred
// bytes; QNetworkReply buffers the whole body, so without a ceiling the responder decides
// the memory cost.
constexpr qint64 kMaxProviderResponseBytes{1024 * 1024};

// How long one request to a provider may take, whichever endpoint it is.
constexpr int kProviderTimeoutMs{15000};

// How often to look for unclaimed tokens: twice per window, at most once a minute.
int unclaimedSweepMs(int windowSeconds)
{
    return qBound(1000, (windowSeconds * 1000) / 2, 60000);
}

// Refuse a response past kMaxProviderResponseBytes while it arrives, and abandon one that
// takes longer than kProviderTimeoutMs. Both mark the reply before aborting it, since an
// aborted reply only reports cancellation. Connected to the reply, so they end with it.
void boundReply(QNetworkReply *reply)
{
    refuseAnswersLargerThan(reply, kMaxProviderResponseBytes);
    QTimer *deadline{new QTimer{reply}};
    deadline->setSingleShot(true);
    QObject::connect(deadline, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("synqtTimedOut", true);
        reply->abort();
    });
    deadline->start(kProviderTimeoutMs);
}

// Why a reply was abandoned or refused, read from the marks boundReply left. Empty when it
// finished normally.
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
    // Always on, unlike the refresh sweep: releasing unclaimed secrets is not optional.
    m_unclaimedTimer = new QTimer{this};
    connect(m_unclaimedTimer, &QTimer::timeout, this, [this]() { releaseUnclaimed(); });
    m_unclaimedTimer->start(unclaimedSweepMs(m_unclaimedWindowSeconds));
}

void OAuthBackend::setUnclaimedWindow(int seconds)
{
    m_unclaimedWindowSeconds = qMax(0, seconds);
    // At least twice per window, and at most once a minute by default.
    m_unclaimedTimer->start(unclaimedSweepMs(m_unclaimedWindowSeconds));
}

void OAuthBackend::releaseUnclaimed()
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    const qint64 window{static_cast<qint64>(m_unclaimedWindowSeconds) * 1000};
    for (auto it{m_tokens.begin()}; it != m_tokens.end();) {
        if (!it->bound && (now - it->storedMs) >= window) {
            // Logged: a login that reached the provider and then had no session to receive
            // it.
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
    // One manager for every call this backend makes, created on first use, with the server
    // egress route its own environment names, never the machine's browser settings (see
    // SynQt::applyEnvironmentProxy).
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
        applyEnvironmentProxy(m_network);
        // The token exchange is a request Qt's flow makes on this manager, out of
        // boundReply()'s reach. The manager applies the same deadline to every request, so
        // that one is ended too.
        m_network->setTransferTimeout(std::chrono::milliseconds{kProviderTimeoutMs});
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
        // requestedScopeTokens, not the joined scope string: setScope is deprecated since
        // 6.11.
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
    // Refused here, the last point before a browser is sent anywhere, which every login
    // passes.
    if (const QString endpoint{insecureEndpoint(*provider)}; !endpoint.isEmpty()) {
        qWarning("SynQt: identity provider '%s' has a plaintext %s endpoint; refusing the "
                 "login rather than sending the secret, the code or the signing keys over "
                 "http", qUtf8Printable(providerName), qUtf8Printable(endpoint));
        result.error = QStringLiteral("insecure provider endpoint");
        return result;
    }
    // An ID token is checked against the configured issuer, and a provider without one
    // skips that check, so such a provider is refused here along with the other unsafe
    // configurations.
    if (provider->useIdToken && provider->issuer.isEmpty()) {
        qWarning("SynQt: identity provider '%s' verifies ID tokens but names no issuer, so "
                 "the iss claim would not be checked at all; set identity.providers.%s."
                 "issuer", qUtf8Printable(providerName), qUtf8Printable(providerName));
        result.error = QStringLiteral("provider names no issuer");
        return result;
    }
    // Bounded, and full only after expirePending: then the oldest pending login makes room.
    // Refusing the new one instead would let whoever keeps the table full turn every visitor
    // away; dropping the oldest costs its visitor a second attempt.
    if (m_pending.size() >= MaxPendingLogins) {
        auto oldest{m_pending.begin()};
        for (auto it{m_pending.begin()}; it != m_pending.end(); ++it) {
            if (it->createdMs < oldest->createdMs) {
                oldest = it;
            }
        }
        qWarning("SynQt: %d logins are in flight; dropping the oldest to start this one",
                 MaxPendingLogins);
        oldest->flow->deleteLater();
        m_pending.erase(oldest);
    }

    QOAuth2AuthorizationCodeFlow *flow{makeFlow(*provider, redirectUri)};
    const QString state{randomToken()};
    flow->setState(state);

    // OpenID Connect: bind the ID token to this request with a nonce in the authorization
    // request, checked on the returned ID token.
    //
    // Through Qt's own nonce setting, not an extra parameter. `NonceMode::Automatic` (the
    // default) already adds a nonce when the scope contains `openid`, and a second
    // parameter would send two values, which RFC 6749 section 3.1 forbids. Setting the mode
    // explicitly and passing Qt this framework's random value keeps exactly one nonce.
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
/// Owned by the backend and deleted once it has answered. It drives the pending login's
/// flow and leaves the tokens with the backend. It reports exactly once, through the
/// caller's `done`.
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
            // OpenID Connect: identity comes from the ID token, verified against the
            // provider JWKS before any claim is trusted.
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
                // An address the provider has not verified names nobody: a mapping hook
                // that grants a scope by address would hand it to whoever registered the
                // address first. No `email_verified` claim at all is also no.
                const QString email{claims.value(QStringLiteral("email")).toString()};
                const bool verified{
                    saysVerified(claims.value(QStringLiteral("email_verified")))};
                identity.insert(QStringLiteral("email"), (email.isEmpty() || !verified)
                                                             ? QVariant{}
                                                             : QVariant{email});
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

        // The subject comes first and is required: the scope mapping, device enrolment and
        // the application all key on it. A profile without one (a misspelled `sub_field`,
        // an unexpected answer) would otherwise sign every such visitor in as the same
        // empty subject.
        const QString subject{profile.value(m_provider.subField).toString()};
        if (subject.isEmpty()) {
            fail(QStringLiteral("userinfo response carried no '%1'").arg(m_provider.subField));
            return;
        }

        m_identity.insert(QStringLiteral("sub"), subject);
        m_identity.insert(QStringLiteral("login"), profile.value(m_provider.loginField));
        m_identity.insert(QStringLiteral("name"), profile.value(m_provider.nameField));
        // A profile that says its address is unverified has none, and the verified primary
        // from the emails endpoint is asked for instead where there is one.
        const QVariant email{statesUnverified(profile) ? QVariant{}
                                                       : profile.value(m_provider.emailField)};
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
            // Copy-initialized, not braced: QJsonArray's initializer_list constructor would
            // take the array as one element (see Topology::topologyFromJson).
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
        // They never leave this engine and are never logged.
        TokenEntry entry;
        entry.providerName = m_pending.providerName;
        entry.accessToken = flow->token();
        entry.refreshToken = flow->refreshToken();
        entry.idToken = flow->idToken();
        const QDateTime expiry{flow->expirationAt()};
        entry.expiresAtMs = expiry.isValid() ? expiry.toMSecsSinceEpoch() : 0;
        // Under the state key and unclaimed. The caller binds a session next;
        // OAuthBackend::releaseUnclaimed handles the case where it never does.
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
        // Retired before the caller is told, so a new exchange started inside `done` finds
        // this one gone. The callback is moved out first.
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
    // The asynchronous form, waited on. A route handler may wait (the identity routes bound
    // how many wait at once); a slot may not, and uses exchangeAsync.
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
        // The loop serves other callers while it spins. See SynQt::TraceScope on detaching.
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

    // Only a state this engine issued and still holds is accepted; unknown or replayed
    // states are rejected before any exchange.
    if (state.isEmpty() || !m_pending.contains(state)) {
        ExchangeResult result;
        result.error = QStringLiteral("invalid or expired state");
        done(result);
        return;
    }
    Pending pending{m_pending.take(state)};
    QOAuth2AuthorizationCodeFlow *flow{pending.flow};

    // Login-CSRF check, here and before the code is spent: here, because the record lives
    // here; before, because exchanging first would hand a real authorization code to
    // whoever sent the callback.
    //
    // The pending record is already taken, so the state is single-use whatever the outcome.
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

    // Nothing is waited on from here. The job answers through `done` when the provider
    // does, then deletes itself.
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
    // Claimed: it now lives and ends with its session, not with the unbound-login window.
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
    // One sweep at a time. Each refresh waits in a nested event loop while the timer keeps
    // firing, so a slow provider would start a second sweep over the same list and spend a
    // refresh token twice, which a rotating provider answers by invalidating both.
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
    // Read by value, never through a held iterator. Everything below waits in a nested
    // event loop that runs other handlers: a completing callback may insert into m_tokens
    // and rehash it, and a session ending may erase from it. So the entry is copied, the
    // wait happens, and the row is looked up again afterwards.
    const auto before{m_tokens.constFind(key)};
    if (before == m_tokens.constEnd() || before->refreshToken.isEmpty()) {
        return false;
    }
    const TokenEntry entry{before.value()};
    const IdentityProviderConfig *provider{m_config.provider(entry.providerName)};
    if (!provider) {
        return false;
    }

    // RFC 6749 section 6: exchange the refresh token for a new access token, server-side.
    // The client secret stays here.
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

    // A sweep runs from a timer, so it may wait, with the same bound as every provider
    // request. A reply abandoned at the deadline reads as an error below and keeps the old
    // entry, which may still be valid.
    boundReply(reply);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    {
        // The loop serves other callers while it spins. See SynQt::TraceScope on detaching.
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

    // Looked up again: during the wait the row may have been rekeyed, replaced by a second
    // sign-in, or erased by a revocation. If it is gone the session ended, and the new
    // tokens are dropped rather than restoring a revoked credential.
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
    // The lifetime the provider gave, or none; never the expired one.
    //
    // `expires_in` is recommended, not required (RFC 6749 section 5.1). Keeping the old
    // value would leave the entry past its threshold, and the sweep would refresh it every
    // interval for the life of the session. Zero is what the exchange writes for an unknown
    // expiry (see exchange()), and refreshExpiring() skips it.
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
    // Answered on `context`, so a job that is gone is not told. The reply is still finished
    // and freed.
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
