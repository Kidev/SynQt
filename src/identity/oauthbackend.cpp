// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "oauthbackend.h"

#include "constanttime.h"
#include "edgereplyhandler.h"
#include "jwksverifier.h"
#include "secrets.h"

#include "proxypolicy.h"

#include <QAbstractOAuth>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QSet>
#include <QOAuth2AuthorizationCodeFlow>
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

// Refuse an answer past kMaxProviderResponseBytes while it is still arriving. Connected to
// the loop rather than to the reply so it dies with the wait, exactly as the deadline does.
void boundResponse(QNetworkReply *reply, QEventLoop *loop)
{
    QObject::connect(reply, &QNetworkReply::downloadProgress, loop,
                     [reply, loop](qint64 received, qint64 total) {
        if (received > kMaxProviderResponseBytes || total > kMaxProviderResponseBytes) {
            reply->abort();
            loop->quit();
        }
    });
}

} // namespace

OAuthBackend::OAuthBackend(IdentityConfig config, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
{
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
    QString nonce;
    if (provider->useIdToken) {
        nonce = randomToken();
        flow->setModifyParametersFunction(
            [nonce](QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant> *parameters) {
                if (stage == QAbstractOAuth::Stage::RequestingAuthorization) {
                    parameters->insert(QStringLiteral("nonce"), nonce);
                }
            });
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

OAuthBackend::ExchangeResult OAuthBackend::exchange(const QString &state, const QString &code,
                                                    const QString &redirectUri,
                                                    const QString &presentedBinding)
{
    Q_UNUSED(redirectUri);  // the pending flow already carries the matching redirect_uri
    ExchangeResult result;

    // Only a state this engine issued (and still holds) is accepted. An unknown or replayed
    // state is rejected before any token exchange.
    if (state.isEmpty() || !m_pending.contains(state)) {
        result.error = QStringLiteral("invalid or expired state");
        return result;
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
        result.error = QStringLiteral("login session mismatch");
        return result;
    }
    result.context = pending.context;

    const IdentityProviderConfig *provider{m_config.provider(pending.providerName)};
    if (!provider) {
        flow->deleteLater();
        result.error = QStringLiteral("unknown provider");
        return result;
    }

    // Drive the token exchange to completion (bounded). A nested loop keeps the caller
    // synchronous; this is a one-shot per-login action.
    QEventLoop loop;
    bool granted{false};
    connect(flow, &QOAuth2AuthorizationCodeFlow::granted, &loop, [&granted, &loop]() {
        granted = true;
        loop.quit();
    });
    connect(flow, &QAbstractOAuth::requestFailed, &loop,
            [&loop](QAbstractOAuth::Error) { loop.quit(); });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);

    auto *handler{qobject_cast<EdgeReplyHandler *>(flow->replyHandler())};
    handler->receiveCallback(QVariantMap{{QStringLiteral("code"), code},
                                         {QStringLiteral("state"), state}});
    loop.exec();

    if (!granted) {
        flow->deleteLater();
        result.error = QStringLiteral("token exchange failed");
        return result;
    }

    QString error;
    const QVariantMap identity{normalizeIdentity(*provider, flow, pending.nonce, &error)};
    if (identity.isEmpty()) {
        flow->deleteLater();
        result.error = error.isEmpty() ? QStringLiteral("identity could not be resolved") : error;
        return result;
    }

    // Store the tokens under the state key (rekeyed to the session id once it exists). The
    // tokens never leave this engine and are never logged.
    TokenEntry entry;
    entry.providerName = pending.providerName;
    entry.accessToken = flow->token();
    entry.refreshToken = flow->refreshToken();
    entry.idToken = flow->idToken();
    const QDateTime expiry{flow->expirationAt()};
    entry.expiresAtMs = expiry.isValid() ? expiry.toMSecsSinceEpoch() : 0;
    m_tokens.insert(state, entry);

    flow->deleteLater();
    result.identity = identity;
    result.tokenKey = state;
    return result;
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
    m_tokens.insert(toKey, it.value());
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

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    boundResponse(reply, &loop);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();

    // See httpGet: an unfinished reply has no error on it, and a partial token response
    // would otherwise be parsed as an answer. Keeping the old entry is the right outcome
    // either way, since it may still have time left on it.
    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        return false;
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
    if (object.contains(QStringLiteral("expires_in"))) {
        const qint64 expiresIn{
            static_cast<qint64>(object.value(QStringLiteral("expires_in")).toDouble())};
        after->expiresAtMs = QDateTime::currentMSecsSinceEpoch() + expiresIn * 1000;
    }
    return true;
}

QByteArray OAuthBackend::httpGet(const QUrl &url, const QString &bearer, QString *error)
{
    QNetworkRequest request{url};
    request.setRawHeader(QByteArrayLiteral("Authorization"), "Bearer " + bearer.toUtf8());
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    QNetworkReply *reply{network()->get(request)};

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    boundResponse(reply, &loop);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();

    // The deadline, read the only way it can be read. A reply the timer walked out on
    // carries no error yet, so asking error() alone takes a half-arrived body for a whole
    // one: here, a truncated profile that parses into an identity missing fields.
    if (!reply->isFinished()) {
        reply->abort();
        if (error) {
            *error = QStringLiteral("the request to %1 timed out")
                         .arg(url.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery));
        }
        reply->deleteLater();
        return {};
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (error) {
            *error = reply->errorString();
        }
        reply->deleteLater();
        return {};
    }
    const QByteArray data{reply->readAll()};
    reply->deleteLater();
    return data;
}

QVariantMap OAuthBackend::normalizeIdentity(const IdentityProviderConfig &provider,
                                            QOAuth2AuthorizationCodeFlow *flow,
                                            const QString &expectedNonce, QString *error)
{
    if (provider.useIdToken) {
        // OpenID Connect: identity from the ID token, whose signature is verified against
        // the provider JWKS before any claim is trusted.
        if (!m_jwks) {
            m_jwks = new JwksVerifier{network(), this};
        }
        const QVariantMap claims{
            m_jwks->verify(flow->idToken(), provider, expectedNonce, error)};
        if (claims.isEmpty()) {
            return {};
        }
        QVariantMap identity;
        identity.insert(QStringLiteral("sub"), claims.value(QStringLiteral("sub")).toString());
        identity.insert(QStringLiteral("login"),
                        claims.value(QStringLiteral("preferred_username")));
        identity.insert(QStringLiteral("name"), claims.value(QStringLiteral("name")));
        const QString email{claims.value(QStringLiteral("email")).toString()};
        identity.insert(QStringLiteral("email"), email.isEmpty() ? QVariant{} : QVariant{email});
        return identity;
    }

    if (provider.userinfoUrl.isEmpty()) {
        if (error) {
            *error = QStringLiteral("provider has no userinfo endpoint");
        }
        return {};
    }
    const QByteArray body{httpGet(provider.userinfoUrl, flow->token(), error)};
    const QJsonDocument document{QJsonDocument::fromJson(body)};
    if (!document.isObject()) {
        if (error) {
            *error = QStringLiteral("userinfo response was not an object");
        }
        return {};
    }
    const QVariantMap profile{document.object().toVariantMap()};

    // The subject, first and required. Everything downstream keys on it: the scope mapping
    // reads it, a device credential is enrolled against it, and an application tells one
    // user from another by it. A profile that carries none (a misspelled `sub_field`, a
    // provider that answered something else) would otherwise sign every such visitor in as
    // the same empty subject, which is one shared account rather than a failed login.
    const QString subject{profile.value(provider.subField).toString()};
    if (subject.isEmpty()) {
        if (error) {
            *error = QStringLiteral("userinfo response carried no '%1'").arg(provider.subField);
        }
        return {};
    }

    QVariantMap identity;
    identity.insert(QStringLiteral("sub"), subject);
    identity.insert(QStringLiteral("login"), profile.value(provider.loginField));
    identity.insert(QStringLiteral("name"), profile.value(provider.nameField));

    QVariant email{profile.value(provider.emailField)};
    if ((email.isNull() || email.toString().isEmpty()) && !provider.emailsUrl.isEmpty()) {
        // GitHub-style fallback: the primary verified address from the emails endpoint.
        const QByteArray emailsBody{httpGet(provider.emailsUrl, flow->token(), nullptr)};
        const QJsonDocument emailsDoc{QJsonDocument::fromJson(emailsBody)};
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
    }
    // Email is nullable: a valid address or a null QVariant, never an empty string.
    identity.insert(QStringLiteral("email"),
                    email.toString().isEmpty() ? QVariant{} : QVariant{email.toString()});
    return identity;
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
