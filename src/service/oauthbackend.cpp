// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "oauthbackend.h"

#include "edgereplyhandler.h"
#include "jwksverifier.h"

#include <QAbstractOAuth>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QOAuth2AuthorizationCodeFlow>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrlQuery>

#include <utility>

namespace SynQt {

namespace {

// A cryptographically random opaque token (state, nonce, ...), hex-encoded.
QString randomToken()
{
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()),
                                          raw.size() / static_cast<int>(sizeof(quint32)));
    return QString::fromLatin1(raw.toHex());
}

} // namespace

OAuthBackend::OAuthBackend(IdentityConfig config, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
{
}

OAuthBackend::~OAuthBackend() = default;

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
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
    }
    QOAuth2AuthorizationCodeFlow *flow{new QOAuth2AuthorizationCodeFlow{
        provider.clientId, provider.authorizeUrl, provider.tokenUrl, m_network, this}};
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
                                              const QString &redirectUri)
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
    pending.createdMs = QDateTime::currentMSecsSinceEpoch();
    m_pending.insert(state, pending);

    result.state = state;
    result.authorizeUrl = authorizeUrl;
    return result;
}

OAuthBackend::ExchangeResult OAuthBackend::exchange(const QString &state, const QString &code,
                                                    const QString &redirectUri)
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
    const qint64 threshold{QDateTime::currentMSecsSinceEpoch()
                           + static_cast<qint64>(marginSeconds) * 1000};
    int refreshed{0};
    // Collect first: refreshOne mutates m_tokens, so do not iterate it while refreshing.
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
    const auto it{m_tokens.find(key)};
    if (it == m_tokens.end() || it->refreshToken.isEmpty()) {
        return false;
    }
    const IdentityProviderConfig *provider{m_config.provider(it->providerName)};
    if (!provider) {
        return false;
    }
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
    }

    // RFC 6749 section 6: exchange the refresh token for a fresh access token, server-side. The
    // client secret stays here; the browser is never involved.
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    body.addQueryItem(QStringLiteral("refresh_token"), it->refreshToken);
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
        m_network->post(request, body.toString(QUrl::FullyEncoded).toUtf8())};

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();

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

    it->accessToken = access;
    // A provider may rotate the refresh token; keep the old one if it does not.
    const QString rotated{object.value(QStringLiteral("refresh_token")).toString()};
    if (!rotated.isEmpty()) {
        it->refreshToken = rotated;
    }
    const QString freshId{object.value(QStringLiteral("id_token")).toString()};
    if (!freshId.isEmpty()) {
        it->idToken = freshId;
    }
    if (object.contains(QStringLiteral("expires_in"))) {
        const qint64 expiresIn{
            static_cast<qint64>(object.value(QStringLiteral("expires_in")).toDouble())};
        it->expiresAtMs = QDateTime::currentMSecsSinceEpoch() + expiresIn * 1000;
    }
    return true;
}

QByteArray OAuthBackend::httpGet(const QUrl &url, const QString &bearer, QString *error)
{
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
    }
    QNetworkRequest request{url};
    request.setRawHeader(QByteArrayLiteral("Authorization"), "Bearer " + bearer.toUtf8());
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    QNetworkReply *reply{m_network->get(request)};

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();

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
            if (!m_network) {
                m_network = new QNetworkAccessManager{this};
            }
            m_jwks = new JwksVerifier{m_network, this};
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

    QVariantMap identity;
    identity.insert(QStringLiteral("sub"), profile.value(provider.subField).toString());
    identity.insert(QStringLiteral("login"), profile.value(provider.loginField));
    identity.insert(QStringLiteral("name"), profile.value(provider.nameField));

    QVariant email{profile.value(provider.emailField)};
    if ((email.isNull() || email.toString().isEmpty()) && !provider.emailsUrl.isEmpty()) {
        // GitHub-style fallback: the primary verified address from the emails endpoint.
        const QByteArray emailsBody{httpGet(provider.emailsUrl, flow->token(), nullptr)};
        const QJsonDocument emailsDoc{QJsonDocument::fromJson(emailsBody)};
        if (emailsDoc.isArray()) {
            const QJsonArray emails{emailsDoc.array()};
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
