// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "identityprovider.h"

#include "identitymapping.h"
#include "oauthbackend.h"
#include "sessionmanager.h"

#include <QDateTime>
#include <QEventLoop>
#include <QHttpHeaders>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QQmlComponent>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrlQuery>
#include <QtQml/qqmlengine.h>

#include <utility>

namespace SynQt {

namespace {

// A cryptographically random opaque token (state, request id, ...), hex-encoded.
QString randomToken()
{
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()),
                                          raw.size() / static_cast<int>(sizeof(quint32)));
    return QString::fromLatin1(raw.toHex());
}

QHttpServerResponse redirectTo(const QString &location,
                               const QList<QByteArray> &setCookies = {})
{
    QHttpServerResponse response{QHttpServerResponse::StatusCode::Found};
    QHttpHeaders headers{response.headers()};
    headers.append(QHttpHeaders::WellKnownHeader::Location, location.toUtf8());
    for (const QByteArray &cookie : setCookies) {
        if (!cookie.isEmpty()) {
            headers.append(QHttpHeaders::WellKnownHeader::SetCookie, cookie);
        }
    }
    response.setHeaders(std::move(headers));
    return response;
}

// The value of a named cookie from a Cookie request header, or empty.
QByteArray cookieValue(const QByteArray &cookieHeader, const QByteArray &name)
{
    const QByteArray prefix{name + "="};
    const QList<QByteArray> parts{cookieHeader.split(';')};
    for (QByteArray part : parts) {
        part = part.trimmed();
        if (part.startsWith(prefix)) {
            return part.mid(prefix.size());
        }
    }
    return {};
}

// Length-constant comparison, so a mismatch does not leak position via timing.
bool constantTimeEquals(const QByteArray &lhs, const QByteArray &rhs)
{
    if (lhs.isEmpty() || lhs.size() != rhs.size()) {
        return false;
    }
    quint8 difference{0};
    for (qsizetype i{0}; i < lhs.size(); ++i) {
        difference |= static_cast<quint8>(lhs.at(i)) ^ static_cast<quint8>(rhs.at(i));
    }
    return difference == 0;
}

// The cookie name that binds a pending login to the browser that started it.
const QByteArray kOauthStateCookie{QByteArrayLiteral("synqt_oauth_state")};

// How long a delegated begin/exchange over the mesh may take before the handler gives up.
constexpr int kRemoteTimeoutMs{20000};

} // namespace

IdentityProvider::IdentityProvider(IdentityConfig config, SessionManager *sessions,
                                   QQmlEngine *engine, QString edgeOrigin, CookiePolicy cookie,
                                   QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_sessions{sessions}
    , m_engine{engine}
    , m_edgeOrigin{std::move(edgeOrigin)}
    , m_cookie{std::move(cookie)}
{
    qmlRegisterType<IdentityMapping>("SynQt", 1, 0, "IdentityMapping");
    if (!m_config.mappingHook.isEmpty() && m_engine) {
        m_mappingComponent = new QQmlComponent{
            m_engine, QUrl::fromLocalFile(m_config.mappingHook), this};
        m_mapping = m_mappingComponent->create();
        if (m_mapping) {
            m_mapping->setParent(this);
        }
    }
    // In-process mode owns the secret-bearing engine. In provider_entity mode the secret and
    // tokens live on the auth entity, so this edge builds no backend and holds no secret.
    if (m_config.providerEntity.isEmpty()) {
        m_backend = new OAuthBackend{m_config, this};
        m_backend->setAutoRefresh(m_config.refreshIntervalSeconds, m_config.refreshMarginSeconds);
    }
}

IdentityProvider::~IdentityProvider() = default;

QString IdentityProvider::loginRoute() const
{
    return m_config.loginRoute;
}

QString IdentityProvider::callbackRoute() const
{
    return m_config.callbackRoute;
}

QString IdentityProvider::logoutRoute() const
{
    return m_config.logoutRoute;
}

void IdentityProvider::setEdgeOrigin(const QString &origin)
{
    m_edgeOrigin = origin;
}

OAuthBackend *IdentityProvider::backend() const
{
    return m_backend;
}

bool IdentityProvider::isRemote() const
{
    return !m_config.providerEntity.isEmpty();
}

void IdentityProvider::attachRemote(QObject *identityReplica)
{
    m_remote = identityReplica;
    // The auth entity's answers connect by name into our receiving slots (the Identity
    // Replica is dynamic, so string-based connect as in SessionManager::attachRemote). As
    // with the session cache, this provider must be destroyed while the Replica is still
    // alive, since the Replica frees its runtime metaobject on destruction.
    connect(identityReplica,
            SIGNAL(beginResult(QString, QString, QString, QString)),
            this, SLOT(onBeginResult(QString, QString, QString, QString)));
    connect(identityReplica,
            SIGNAL(exchangeResult(QString, QString, QString)),
            this, SLOT(onExchangeResult(QString, QString, QString)));
}

void IdentityProvider::onBeginResult(const QString &requestId, const QString &state,
                                     const QString &authorizeUrl, const QString &error)
{
    BeginOutcome outcome;
    outcome.state = state;
    outcome.authorizeUrl = authorizeUrl;
    outcome.error = error;
    m_beginResults.insert(requestId, outcome);
    emit beginArrived(requestId);
}

void IdentityProvider::onExchangeResult(const QString &requestId, const QString &identityJson,
                                        const QString &error)
{
    ExchangeOutcome outcome;
    outcome.identity = identityJson.isEmpty()
        ? QVariantMap{}
        : QJsonDocument::fromJson(identityJson.toUtf8()).object().toVariantMap();
    outcome.error = error;
    m_exchangeResults.insert(requestId, outcome);
    emit exchangeArrived(requestId);
}

IdentityProvider::BeginOutcome IdentityProvider::beginLogin(const QString &providerName)
{
    const QString redirectUri{m_edgeOrigin + m_config.callbackRoute};
    if (!isRemote()) {
        const OAuthBackend::BeginResult result{m_backend->begin(providerName, redirectUri)};
        return BeginOutcome{result.state, result.authorizeUrl.toString(QUrl::FullyEncoded),
                            result.error};
    }
    if (!m_remote) {
        return BeginOutcome{QString{}, QString{}, QStringLiteral("auth entity not connected")};
    }

    // Delegate to the auth entity: invoke the slot, then wait (bounded) for the correlated
    // beginResult signal. The nested loop keeps the route handler synchronous.
    const QString requestId{randomToken()};
    QEventLoop loop;
    connect(this, &IdentityProvider::beginArrived, &loop, [&loop, requestId](const QString &id) {
        if (id == requestId) {
            loop.quit();
        }
    });
    QTimer::singleShot(kRemoteTimeoutMs, &loop, &QEventLoop::quit);
    QMetaObject::invokeMethod(m_remote, "beginLogin", Q_ARG(QString, requestId),
                              Q_ARG(QString, providerName), Q_ARG(QString, redirectUri));
    loop.exec();

    if (!m_beginResults.contains(requestId)) {
        return BeginOutcome{QString{}, QString{}, QStringLiteral("auth entity timed out")};
    }
    return m_beginResults.take(requestId);
}

IdentityProvider::ExchangeOutcome IdentityProvider::exchangeCode(const QString &state,
                                                                 const QString &code)
{
    const QString redirectUri{m_edgeOrigin + m_config.callbackRoute};
    if (!isRemote()) {
        const OAuthBackend::ExchangeResult result{m_backend->exchange(state, code, redirectUri)};
        return ExchangeOutcome{result.identity, result.tokenKey, result.error};
    }
    if (!m_remote) {
        return ExchangeOutcome{QVariantMap{}, QString{},
                               QStringLiteral("auth entity not connected")};
    }

    const QString requestId{randomToken()};
    QEventLoop loop;
    connect(this, &IdentityProvider::exchangeArrived, &loop,
            [&loop, requestId](const QString &id) {
                if (id == requestId) {
                    loop.quit();
                }
            });
    QTimer::singleShot(kRemoteTimeoutMs, &loop, &QEventLoop::quit);
    QMetaObject::invokeMethod(m_remote, "exchangeCode", Q_ARG(QString, requestId),
                              Q_ARG(QString, state), Q_ARG(QString, code),
                              Q_ARG(QString, redirectUri));
    loop.exec();

    if (!m_exchangeResults.contains(requestId)) {
        return ExchangeOutcome{QVariantMap{}, QString{}, QStringLiteral("auth entity timed out")};
    }
    ExchangeOutcome outcome{m_exchangeResults.take(requestId)};
    // The tokens are held on the auth entity under the state key until the session exists.
    outcome.tokenKey = state;
    return outcome;
}

void IdentityProvider::bindRemoteSession(const QString &state, const QByteArray &sessionId)
{
    if (m_remote) {
        QMetaObject::invokeMethod(m_remote, "bindSession", Q_ARG(QString, state),
                                  Q_ARG(QString, QString::fromLatin1(sessionId)));
    }
}

void IdentityProvider::releaseRemoteTokens(const QByteArray &sessionId)
{
    if (m_remote) {
        QMetaObject::invokeMethod(m_remote, "releaseSession",
                                  Q_ARG(QString, QString::fromLatin1(sessionId)));
    }
}

QVariantMap IdentityProvider::tokensForSession(const QByteArray &sessionId) const
{
    if (m_backend) {
        return m_backend->tokens(QString::fromLatin1(sessionId));
    }
    return {};  // provider_entity mode: tokens live only on the auth entity
}

QHttpServerResponse IdentityProvider::handleLogin(const QHttpServerRequest &request)
{
    expirePending();
    const QUrlQuery query{request.url().query()};
    QString providerName{query.queryItemValue(QStringLiteral("provider"))};
    if (providerName.isEmpty() && !m_config.providers.isEmpty()) {
        providerName = m_config.providers.first().name;
    }

    const BeginOutcome begin{beginLogin(providerName)};
    if (!begin.error.isEmpty()) {
        // Preserve the specific status codes the browser flow relies on.
        if (begin.error == QLatin1String("unknown provider")) {
            return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                       QByteArrayLiteral("unknown provider"),
                                       QHttpServerResponse::StatusCode::NotFound};
        }
        if (begin.error == QLatin1String("dev stub provider is disabled")) {
            return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                       QByteArrayLiteral("dev stub provider is disabled"),
                                       QHttpServerResponse::StatusCode::Forbidden};
        }
        return QHttpServerResponse{QHttpServerResponse::StatusCode::InternalServerError};
    }
    if (begin.state.isEmpty() || begin.authorizeUrl.isEmpty()) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::InternalServerError};
    }

    // Bind this pending login to the browser that started it: a random value set as a
    // cookie now and required to match on the callback (defeats login CSRF / fixation).
    const QString csrfToken{randomToken()};
    PendingLogin pending;
    pending.csrfToken = csrfToken;
    pending.createdMs = QDateTime::currentMSecsSinceEpoch();
    m_pending.insert(begin.state, pending);

    return redirectTo(begin.authorizeUrl, {buildStateCookie(csrfToken.toUtf8(), false)});
}

QHttpServerResponse IdentityProvider::handleCallback(const QHttpServerRequest &request)
{
    const QUrlQuery query{request.url().query()};
    const QString code{query.queryItemValue(QStringLiteral("code"))};
    const QString state{query.queryItemValue(QStringLiteral("state"))};

    // Framework state verification: only a state this edge issued (and still holds) is
    // accepted. An unknown or replayed state is rejected before any token exchange.
    if (state.isEmpty() || !m_pending.contains(state)) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("invalid or expired state"),
                                   QHttpServerResponse::StatusCode::BadRequest};
    }
    PendingLogin pending{m_pending.take(state)};

    // Login-CSRF defense: the callback must come from the same browser that started the
    // login, proven by the state cookie set then. A state alone is not enough; an
    // attacker could hand a victim a valid state and their own authorization code.
    const QByteArray presentedCsrf{cookieValue(request.value("Cookie"), kOauthStateCookie)};
    if (!constantTimeEquals(presentedCsrf, pending.csrfToken.toUtf8())) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("login session mismatch"),
                                   QHttpServerResponse::StatusCode::BadRequest};
    }

    const ExchangeOutcome exchange{exchangeCode(state, code)};
    if (exchange.identity.isEmpty()) {
        return redirectTo(m_config.appRoute, {buildStateCookie(QByteArray{}, true)});
    }

    const QString scope{mapScope(exchange.identity)};
    const QByteArray sessionId{m_sessions->createSession(scope, exchange.identity)};

    // Move the tokens under the stable session id so refresh can find them, and keep them
    // where they already are: on the edge (in-process) or the auth entity (provider_entity).
    if (isRemote()) {
        bindRemoteSession(exchange.tokenKey, sessionId);
    } else {
        m_backend->rekeyTokens(exchange.tokenKey, QString::fromLatin1(sessionId));
    }

    // Set the session cookie and clear the now-consumed login-state cookie.
    return redirectTo(m_config.appRoute,
                      {buildCookie(sessionId), buildStateCookie(QByteArray{}, true)});
}

QHttpServerResponse IdentityProvider::handleLogout(const QHttpServerRequest &request)
{
    const QByteArray prefix{m_cookie.name.toUtf8() + "="};
    const QList<QByteArray> parts{request.value("Cookie").split(';')};
    for (QByteArray part : parts) {
        part = part.trimmed();
        if (part.startsWith(prefix)) {
            const QByteArray sessionId{part.mid(prefix.size())};
            m_sessions->revoke(sessionId);
            if (m_backend) {
                m_backend->releaseTokens(QString::fromLatin1(sessionId));
            } else {
                releaseRemoteTokens(sessionId);
            }
        }
    }
    // Expire the cookie.
    QByteArray expired{m_cookie.name.toUtf8() + "=; HttpOnly; Path=/; Max-Age=0"};
    return redirectTo(m_config.appRoute, {expired});
}

QString IdentityProvider::mapScope(const QVariantMap &identity)
{
    if (m_mapping) {
        QVariant result;
        if (QMetaObject::invokeMethod(m_mapping, "scopeFor", Qt::DirectConnection,
                                      Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, QVariant{identity}))) {
            const QString scope{result.toString()};
            if (!scope.isEmpty()) {
                return scope;
            }
        }
    }
    return QStringLiteral("user");  // any successfully authenticated user
}

QByteArray IdentityProvider::buildStateCookie(const QByteArray &value, bool expire) const
{
    // SameSite=Lax so the cookie rides the top-level GET navigation back from the provider
    // to the callback, but not a cross-site subrequest.
    QByteArray cookie{kOauthStateCookie + "=" + value + "; HttpOnly; SameSite=Lax; Path=/"};
    if (m_cookie.secure) {
        cookie += "; Secure";
    }
    if (expire) {
        cookie += "; Max-Age=0";
    }
    return cookie;
}

QByteArray IdentityProvider::buildCookie(const QByteArray &token) const
{
    QByteArray cookie{m_cookie.name.toUtf8() + "=" + token + "; HttpOnly; Path=/"};
    if (m_cookie.sameSiteNone) {
        cookie += "; SameSite=None; Secure";
    } else {
        cookie += "; SameSite=Lax";
        if (m_cookie.secure) {
            cookie += "; Secure";
        }
    }
    return cookie;
}

void IdentityProvider::expirePending()
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    for (auto it{m_pending.begin()}; it != m_pending.end();) {
        if (now - it->createdMs > 5 * 60 * 1000) {  // a login has 5 minutes to complete
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace SynQt
