// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "identityprovider.h"

#include "identitymapping.h"
#include "oauthbackend.h"
#include "sessionmanager.h"

#include <QCryptographicHash>
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

// How long a desktop claim code may stand for its session. This is a machine-to-machine hop
// that happens the instant the loopback listener is hit, not a human step, so it is short
// on purpose: the code has already been written into the system browser's history by the
// time it exists, and its whole defence is being useless by the time anyone reads it back.
// Clamped rather than trusted, because the configured value can only make it worse.
qint64 claimTtlMs(const IdentityConfig &config)
{
    return 1000 * qBound(1, config.claimTtlSeconds, 300);
}

// A base64url S256 digest is 43 characters, and nothing else is accepted: pinning the shape
// here means a caller cannot register a challenge that no verifier can ever match (which
// would be a claim code nobody can collect) or one short enough to guess.
constexpr qsizetype kChallengeLength{43};

// The nonce the native client matches the loopback arrival against travels back through a
// URL, so it is bounded rather than reflected at whatever length was sent.
constexpr qsizetype kMaxReturnStateLength{128};

bool isBase64UrlDigest(const QString &value)
{
    if (value.size() != kChallengeLength) {
        return false;
    }
    for (const QChar character : value) {
        const bool allowed{character.isLetterOrNumber() && character.unicode() < 128};
        if (!allowed && character != QLatin1Char('-') && character != QLatin1Char('_')) {
            return false;
        }
    }
    return true;
}

/// Whether a `return` URL may be redirected to at the end of a desktop login.
///
/// This is the single most dangerous line in the desktop flow. Whatever passes here is
/// where a freshly authenticated visitor's browser gets sent, so anything short of exact is
/// an open redirect that hands out sessions. It is an allowlist of one shape, not a filter
/// of known-bad ones.
///
///  - `http` only, and only to the loopback literals. Not `localhost`, which is a name and
///    can be pointed elsewhere by a hosts file (RFC 8252 says the same).
///  - No userinfo, because `http://127.0.0.1@evil.example/` has host `evil.example` and
///    reads to a human as loopback. The host check alone already refuses it. The userinfo
///    check is there so the refusal does not depend on getting the parse right.
///  - No path beyond `/`, no query, no fragment. The redirect appends its own query, and a
///    caller-supplied one is a way to smuggle parameters past that.
bool isLoopbackReturn(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != QLatin1String("http")) {
        return false;
    }
    if (!url.userInfo().isEmpty()) {
        return false;
    }
    const QString host{url.host()};
    if (host != QLatin1String("127.0.0.1") && host != QLatin1String("::1")) {
        return false;
    }
    if (url.port() < 1 || url.port() > 65535) {
        return false;
    }
    if (!url.path().isEmpty() && url.path() != QLatin1String("/")) {
        return false;
    }
    return !url.hasQuery() && !url.hasFragment();
}

QHttpServerResponse notFound()
{
    // One answer for every way a claim can fail: unknown code, expired code, code already
    // spent, wrong verifier. Telling them apart would tell an attacker which half of a
    // guess was right.
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
}

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
        // Asked before creating: create() on a component that failed to compile prints
        // its own "Component is not ready" and says nothing about which file or why.
        m_mapping = m_mappingComponent->isReady() ? m_mappingComponent->create() : nullptr;
        if (m_mapping) {
            m_mapping->setParent(this);
        } else {
            // Loud, because the failure is quiet otherwise: without the hook every
            // authenticated session gets the default scope, which is a permissions change
            // nobody asked for. The hook's own file and QML diagnostic, nothing from the
            // provider payload it would have read.
            qWarning("SynQt: identity mapping hook %s failed to load: %s; every session "
                     "gets the default scope until it does",
                     qUtf8Printable(m_config.mappingHook),
                     qUtf8Printable(m_mappingComponent->errorString()));
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

QString IdentityProvider::claimRoute() const
{
    QString route{m_config.loginRoute};
    while (route.endsWith(QLatin1Char('/'))) {
        route.chop(1);
    }
    return route + QStringLiteral("/claim");
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
    // The auth entity's answers connect by name into this object's receiving slots (the Identity
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

void IdentityProvider::forgetSession(const QByteArray &sessionId)
{
    if (m_backend) {
        m_backend->releaseTokens(QString::fromLatin1(sessionId));
    } else {
        releaseRemoteTokens(sessionId);
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
    expireClaims();
    const QUrlQuery query{request.url().query()};
    QString providerName{query.queryItemValue(QStringLiteral("provider"))};
    if (providerName.isEmpty() && !m_config.providers.isEmpty()) {
        providerName = m_config.providers.first().name;
    }

    // The desktop half, decided before a single byte goes to the provider. A login that
    // asks for a loopback answer and does not fully qualify for one is refused outright
    // rather than quietly downgraded to the browser flow. Downgrading would sign somebody
    // in on a machine whose app is still waiting, and set a cookie in a browser that is not
    // the app. Refusing is the only outcome that leaves nothing behind.
    QString returnUrl;
    const QString requestedReturn{query.queryItemValue(QStringLiteral("return"),
                                                       QUrl::FullyDecoded)};
    const QString returnState{query.queryItemValue(QStringLiteral("return_state"),
                                                   QUrl::FullyDecoded)};
    const QString returnChallenge{query.queryItemValue(QStringLiteral("return_challenge"),
                                                       QUrl::FullyDecoded)};
    if (!requestedReturn.isEmpty()) {
        if (!m_config.allowDesktopLogin) {
            return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                       QByteArrayLiteral("desktop login is not enabled"),
                                       QHttpServerResponse::StatusCode::BadRequest};
        }
        if (!isLoopbackReturn(QUrl{requestedReturn, QUrl::StrictMode})
            || returnState.isEmpty() || returnState.size() > kMaxReturnStateLength
            || !isBase64UrlDigest(returnChallenge)) {
            return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                       QByteArrayLiteral("invalid return"),
                                       QHttpServerResponse::StatusCode::BadRequest};
        }
        returnUrl = requestedReturn;
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
    pending.returnUrl = returnUrl;
    pending.returnState = returnState;
    pending.returnChallenge = returnChallenge;
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
        if (!pending.returnUrl.isEmpty()) {
            // Tell the waiting desktop client it failed. Without this the app sits on its
            // loopback listener until the timeout with nothing to report, which reads to
            // the visitor as a sign-in that hung rather than one that was refused.
            return loopbackRedirect(pending, QString{}, QStringLiteral("access_denied"));
        }
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

    if (!pending.returnUrl.isEmpty()) {
        // A desktop login ends here, and deliberately not with a cookie: the system browser
        // is not the app. Leaving it signed in would put a live session in a browser the
        // visitor did not sign in with, on a machine that may not be theirs alone, and
        // nothing would ever end it. What crosses the loopback is a code that stands for the
        // session for the next minute, exchangeable once, by whoever holds the verifier.
        const QString claimCode{randomToken()};
        PendingClaim claim;
        claim.sessionId = sessionId;
        claim.challenge = pending.returnChallenge;
        claim.createdMs = QDateTime::currentMSecsSinceEpoch();
        m_claims.insert(claimCode, claim);
        return loopbackRedirect(pending, claimCode, QString{});
    }

    // Set the session cookie and clear the now-consumed login-state cookie.
    return redirectTo(m_config.appRoute,
                      {buildCookie(sessionId), buildStateCookie(QByteArray{}, true)});
}

QHttpServerResponse IdentityProvider::loopbackRedirect(const PendingLogin &pending,
                                                       const QString &code,
                                                       const QString &error) const
{
    // Built through QUrl rather than by concatenation. The URL itself was validated to
    // carry no query of its own at login, and the state is the caller's own string, so the
    // encoding is what keeps it a value rather than a second parameter.
    QUrl target{pending.returnUrl, QUrl::StrictMode};
    QUrlQuery query;
    if (!code.isEmpty()) {
        query.addQueryItem(QStringLiteral("code"), code);
    }
    if (!error.isEmpty()) {
        query.addQueryItem(QStringLiteral("error"), error);
    }
    query.addQueryItem(QStringLiteral("state"), pending.returnState);
    target.setQuery(query);
    return redirectTo(target.toString(QUrl::FullyEncoded),
                      {buildStateCookie(QByteArray{}, true)});
}

QHttpServerResponse IdentityProvider::handleClaim(const QHttpServerRequest &request)
{
    expireClaims();
    if (!m_config.allowDesktopLogin) {
        return notFound();
    }
    // No browser has any business here. The browser flow ends with a cookie and never
    // claims. Refusing anything that carries an Origin puts this endpoint out of reach of
    // page script altogether, rather than relying on the code being unguessable.
    if (!request.value("Origin").isEmpty()) {
        return notFound();
    }

    // A code and a verifier are 64 hex characters each. Anything past this is not a claim,
    // and refusing it before the decode keeps a large body from being turned into a QString
    // and parsed as a query. (What QHttpServer buffered before reaching here is its own
    // affair. This is the part the edge decides.)
    constexpr qsizetype kMaxClaimBody{1024};
    if (request.body().size() > kMaxClaimBody) {
        return notFound();
    }
    const QUrlQuery body{QString::fromUtf8(request.body())};
    const QString code{body.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded)};
    const QString verifier{body.queryItemValue(QStringLiteral("verifier"),
                                               QUrl::FullyDecoded)};
    if (code.isEmpty() || !m_claims.contains(code)) {
        return notFound();
    }

    // Taken out before it is checked, so a wrong verifier spends the code rather than
    // leaving it there to be tried again. One code, one attempt.
    const PendingClaim claim{m_claims.take(code)};
    if (QDateTime::currentMSecsSinceEpoch() - claim.createdMs > claimTtlMs(m_config)) {
        return notFound();
    }
    const QByteArray digest{QCryptographicHash::hash(verifier.toUtf8(),
                                                     QCryptographicHash::Sha256)
                                .toBase64(QByteArray::Base64UrlEncoding
                                          | QByteArray::OmitTrailingEquals)};
    if (!constantTimeEquals(digest, claim.challenge.toUtf8())) {
        return notFound();
    }

    QJsonObject answer;
    answer.insert(QStringLiteral("session"), QString::fromLatin1(claim.sessionId));
    answer.insert(QStringLiteral("cookie_name"), m_cookie.name);
    QHttpServerResponse response{QJsonDocument{answer}.toJson(QJsonDocument::Compact)};
    QHttpHeaders headers{response.headers()};
    headers.append(QHttpHeaders::WellKnownHeader::ContentType,
                   QByteArrayLiteral("application/json"));
    // The body is a live credential. Nothing between here and the app may keep a copy.
    headers.append(QHttpHeaders::WellKnownHeader::CacheControl,
                   QByteArrayLiteral("no-store"));
    response.setHeaders(std::move(headers));
    return response;
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
        // This is the cookie the measurement in tests/split-origin singles out. It is set on
        // the callback, a top-level navigation onto the edge, so marking it `Partitioned`
        // files it under the edge's partition where the client site can never read it. The
        // attribute is therefore absent here for a sharper reason than in WebEdge, and it
        // cannot be added until the callback hands the session back through the client
        // context instead of setting it here.
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

void IdentityProvider::expireClaims()
{
    // Swept on the way in to the two routes that can add one, so an uncollected code cannot
    // outlive its minute even on an edge nobody signs into again. A claim that expires here
    // takes nothing with it: the session it stood for is a real session, and it lives or
    // expires on the session manager's own terms.
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    const qint64 ttl{claimTtlMs(m_config)};
    for (auto it{m_claims.begin()}; it != m_claims.end();) {
        if (now - it->createdMs > ttl) {
            it = m_claims.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace SynQt
