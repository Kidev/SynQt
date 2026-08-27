// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "identityprovider.h"

#include "claimstore.h"
#include "clientaddress.h"
#include "cookies.h"
#include "desktoproutes.h"
#include "deviceregistry.h"
#include "identitymapping.h"
#include "oauthbackend.h"
#include "ratewindow.h"
#include "secrets.h"
#include "sessionmanager.h"

#include <QDateTime>
#include "tracescope.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaObject>
#include <QQmlComponent>
#include <QScopeGuard>
#include <QTimer>
#include <QUrlQuery>
#include <QtQml/qqmlengine.h>

#include <algorithm>
#include <utility>

// Asks the running thread for its stack size, which Qt has no API for. The nesting bound
// below depends on this answer. Each branch is guarded to its platform.
#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(Q_OS_UNIX)
#  include <pthread.h>
#endif

namespace SynQt {

namespace {

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

// The cookie name that binds a pending login to the browser that started it.
const QByteArray kOauthStateCookie{QByteArrayLiteral("synqt_oauth_state")};

// A base64url S256 digest is exactly 43 characters. Anything else is refused, so no
// challenge can be registered that no verifier matches, or that is short enough to guess.
constexpr qsizetype kChallengeLength{43};

// The native client's loopback nonce travels back in a URL, so its length is bounded.
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
/// Whatever passes here receives a freshly authenticated browser, so anything short of
/// exact is an open redirect that hands out sessions. It allows one shape only:
///
/// - `http`, only to the loopback literals. Not `localhost`, which a hosts file can
///   redirect (RFC 8252 agrees).
/// - No userinfo: `http://127.0.0.1@evil.example/` has host `evil.example`. The host check
///   already refuses it; this check does not depend on the parse.
/// - No path beyond `/`, no query, no fragment. The redirect adds its own query, and a
///   caller's could smuggle parameters.
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
    // One answer for every claim failure (unknown, expired, spent, wrong verifier), so an
    // attacker cannot tell which half of a guess was right.
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
}

/// The "too fast" answer, distinct from the one above.
///
/// It is decided before the credential is read, so it reveals nothing, and it must be
/// distinguishable: a client that read it as "credential dead" would delete the visitor's
/// stored sign-in because of someone else behind the same address. `Retry-After` is the
/// rest of the window.
/// The fixed window a visitor's requests are counted in (overVisitorLimit).
constexpr qint64 kVisitorWindowMs{60 * 1000};

QHttpServerResponse tooManyRequests(qint64 retryAfterMs)
{
    QHttpServerResponse response{QHttpServerResponse::StatusCode::TooManyRequests};
    QHttpHeaders headers{response.headers()};
    headers.append(QHttpHeaders::WellKnownHeader::RetryAfter,
                   QByteArray::number(retryAfterSeconds(retryAfterMs)));
    response.setHeaders(std::move(headers));
    return response;
}

// How long a delegated begin/exchange over the mesh may take before the handler gives up.
constexpr int kRemoteTimeoutMs{20000};

/// How many answers this edge may wait on at once, from any source.
///
/// Each wait is a nested QEventLoop that keeps serving, so a second waiting request nests
/// inside the first. The login route is open to anyone, so without a ceiling the nesting
/// depth is set by the caller until the stack runs out. The ceiling is far above real
/// concurrent logins, since each wait lasts one round trip (to the auth entity or the
/// identity provider).
constexpr int kMaxConcurrentWaits{64};

/// The stack size to assume when the platform does not report one: the smallest SynQt runs
/// on (a Windows thread gets 1 MB; the Linux and macOS main thread gets 8 MB).
constexpr quintptr kAssumedStackBytes{1024 * 1024};

/// The share of a thread's stack the nesting may use: one quarter.
///
/// The count above cannot bound stack use alone, because the cost of a level depends on
/// Qt's call chain and the compiler. Sixty-four levels fit 8 MB but not the 1 MB Windows
/// gives a thread. So the stack size is read and a quarter of it is the budget; where the
/// stack is large the count still decides. The refusal is the same either way.
constexpr quintptr kStackShareForNesting{4};

/// The size of the running thread's stack, or zero when the platform will not say.
quintptr threadStackBytes()
{
#if defined(Q_OS_WIN)
    ULONG_PTR low{0};
    ULONG_PTR high{0};
    GetCurrentThreadStackLimits(&low, &high);
    return static_cast<quintptr>(high - low);
#elif defined(Q_OS_DARWIN)
    return static_cast<quintptr>(pthread_get_stacksize_np(pthread_self()));
#elif defined(Q_OS_LINUX)
    pthread_attr_t attributes{};
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
        return quintptr{0};
    }
    void *base{nullptr};
    size_t size{0};
    const bool known{pthread_attr_getstack(&attributes, &base, &size) == 0};
    pthread_attr_destroy(&attributes);
    if (!known) {
        return quintptr{0};
    }
    return static_cast<quintptr>(size);
#else
    return quintptr{0};
#endif
}

/// How much stack the nesting rooted at the running thread may spend.
quintptr nestingStackBudget()
{
    const quintptr stack{threadStackBytes()};
    return ((stack > 0) ? stack : kAssumedStackBytes) / kStackShareForNesting;
}

/// How far apart two stack frames are, whichever way this platform grows its stack.
quintptr stackSpent(quintptr outermost, quintptr here)
{
    return (here > outermost) ? (here - outermost) : (outermost - here);
}

} // namespace

IdentityProvider::WaitScope::WaitScope(WaitState *state)
    : m_state{state}
{
    if (m_state->count >= kMaxConcurrentWaits) {
        return;
    }

    // `this` is a local of the frame about to wait, so its address marks that frame. The
    // outermost wait records its own address and reads the thread's budget; nested waits
    // are measured from it.
    const quintptr frame{reinterpret_cast<quintptr>(this)};
    if (m_state->count == 0) {
        m_state->outermostFrame = frame;
        m_state->budget = nestingStackBudget();
    } else if (stackSpent(m_state->outermostFrame, frame) >= m_state->budget) {
        return;
    }
    ++m_state->count;
    m_taken = true;
}

IdentityProvider::WaitScope::~WaitScope()
{
    if (m_taken) {
        --m_state->count;
    }
}

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
        // Checked before create(), whose own "Component is not ready" names neither the
        // file nor the reason.
        m_mapping = m_mappingComponent->isReady() ? m_mappingComponent->create() : nullptr;
        if (m_mapping) {
            m_mapping->setParent(this);
        } else {
            // Logged, because without the hook no session gets a scope and every login is
            // refused until the file loads. The edge stays up. The message has the hook's
            // file and QML diagnostic, nothing from the provider payload.
            qWarning("SynQt: identity mapping hook %s failed to load: %s; every login is "
                     "refused until it does",
                     qUtf8Printable(m_config.mappingHook),
                     qUtf8Printable(m_mappingComponent->errorString()));
        }
    }
    // In-process mode owns the secret-bearing engine. In provider_entity mode the secret
    // and tokens are on the auth entity, so this edge has no backend and no secret.
    if (m_config.providerEntity.isEmpty()) {
        m_backend = new OAuthBackend{m_config, this};
        m_backend->setAutoRefresh(m_config.refreshIntervalSeconds, m_config.refreshMarginSeconds);
    }

    // Staying signed in across relaunches, only when the project asked for it and builds a
    // desktop client: enrolment happens at the claim exchange, which only a native client
    // reaches.
    if (m_config.device.enabled && m_config.allowDesktopLogin) {
        auto *registry{new DeviceRegistry{m_config.device, this}};
        QString error;
        if (registry->open(&error)) {
            m_devices = registry;
            connect(m_devices, &DeviceRegistry::reuseDetected,
                    this, &IdentityProvider::onReuseDetected);
        } else {
            // Logged, then disabled. Refusing to start over an unreachable device store
            // would stop everyone from signing in; disabling it only makes desktop users
            // sign in each launch. The message names the store and the reason.
            delete registry;
            qWarning("SynQt: identity.desktop_session is 'device' and the store would not "
                     "open (%s), so desktop clients will sign in once per launch. No "
                     "credential is persisted anywhere.",
                     qUtf8Printable(error));
        }
    }
}

IdentityProvider::~IdentityProvider() = default;

void IdentityProvider::setClientAddress(const ClientAddress *resolver)
{
    m_clientAddress = resolver;
}

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
    return desktopClaimRoute(m_config.loginRoute);
}

QString IdentityProvider::deviceRoute() const
{
    return desktopDeviceRoute(m_config.loginRoute);
}

DeviceRegistry *IdentityProvider::devices() const
{
    return m_devices;
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
    // The auth entity's answers connect by name to this object's slots (the Identity
    // Replica is dynamic, as in SessionManager::attachRemote). This provider must be
    // destroyed while the Replica is alive, since the Replica frees its runtime metaobject
    // when destroyed.
    connect(identityReplica,
            SIGNAL(beginResult(QString, QString, QString, QString)),
            this, SLOT(onBeginResult(QString, QString, QString, QString)));
    connect(identityReplica,
            SIGNAL(exchangeResult(QString, QString, QString, QString)),
            this, SLOT(onExchangeResult(QString, QString, QString, QString)));
    connect(identityReplica,
            SIGNAL(claimResult(QString, QString)),
            this, SLOT(onClaimResult(QString, QString)));
}

void IdentityProvider::onBeginResult(const QString &requestId, const QString &state,
                                     const QString &authorizeUrl, const QString &error)
{
    if (!m_awaited.contains(requestId)) {
        return;  // nobody is waiting on this one. See m_awaited
    }
    BeginOutcome outcome;
    outcome.state = state;
    outcome.authorizeUrl = authorizeUrl;
    outcome.error = error;
    m_beginResults.insert(requestId, outcome);
    emit beginArrived(requestId);
}

void IdentityProvider::onClaimResult(const QString &requestId, const QString &sessionId)
{
    if (!m_awaited.contains(requestId)) {
        return;  // nobody is waiting on this one. See m_awaited
    }
    m_claimResults.insert(requestId, sessionId.toLatin1());
    emit claimArrived(requestId);
}

void IdentityProvider::onExchangeResult(const QString &requestId, const QString &identityJson,
                                        const QString &context, const QString &error)
{
    if (!m_awaited.contains(requestId)) {
        return;  // nobody is waiting on this one. See m_awaited
    }
    ExchangeOutcome outcome;
    outcome.identity = identityJson.isEmpty()
        ? QVariantMap{}
        : QJsonDocument::fromJson(identityJson.toUtf8()).object().toVariantMap();
    outcome.context = context;
    outcome.error = error;
    m_exchangeResults.insert(requestId, outcome);
    emit exchangeArrived(requestId);
}

IdentityProvider::BeginOutcome IdentityProvider::beginLogin(const QString &providerName,
                                                            const QString &binding,
                                                            const QString &context)
{
    const QString redirectUri{m_edgeOrigin + m_config.callbackRoute};
    if (!isRemote()) {
        const OAuthBackend::BeginResult result{
            m_backend->begin(providerName, redirectUri, binding, context)};
        return BeginOutcome{result.state, result.authorizeUrl.toString(QUrl::FullyEncoded),
                            result.error};
    }
    if (!m_remote) {
        return BeginOutcome{QString{}, QString{}, QStringLiteral("auth entity not connected")};
    }
    const WaitScope wait{&m_waits};
    if (!wait.isTaken()) {
        return BeginOutcome{QString{}, QString{},
                            QStringLiteral("too many logins waiting on the auth entity")};
    }

    // Delegate to the auth entity: invoke the slot, then wait (bounded) for the correlated
    // beginResult signal. The nested loop keeps the route handler synchronous.
    const QString requestId{randomToken()};
    const AwaitScope awaited{&m_awaited, requestId};
    const auto forget{qScopeGuard([this, requestId]() { m_beginResults.remove(requestId); })};
    QEventLoop loop;
    connect(this, &IdentityProvider::beginArrived, &loop, [&loop, requestId](const QString &id) {
        if (id == requestId) {
            loop.quit();
        }
    });
    QTimer::singleShot(kRemoteTimeoutMs, &loop, &QEventLoop::quit);
    QMetaObject::invokeMethod(m_remote, "beginLogin", Q_ARG(QString, requestId),
                              Q_ARG(QString, providerName), Q_ARG(QString, redirectUri),
                              Q_ARG(QString, binding), Q_ARG(QString, context));
    {
        // The loop serves other callers while it spins. See SynQt::TraceScope on detaching.
        const SynQt::TraceScope untraced{SynQt::TraceContext{}};
        loop.exec();
    }

    if (!m_beginResults.contains(requestId)) {
        return BeginOutcome{QString{}, QString{}, QStringLiteral("auth entity timed out")};
    }
    return m_beginResults.take(requestId);
}

IdentityProvider::ExchangeOutcome IdentityProvider::exchangeCode(const QString &state,
                                                                 const QString &code,
                                                                 const QString &presentedBinding)
{
    const QString redirectUri{m_edgeOrigin + m_config.callbackRoute};
    // The ceiling covers both identity modes, because both wait in a nested event loop. In
    // provider_entity mode the wait is for the auth entity. In process it is
    // `OAuthBackend::exchange` around the token exchange, which is reachable: the callback
    // route is open, a valid state passes the first check, and up to
    // `OAuthBackend::MaxPendingLogins` can be in flight at once.
    const WaitScope wait{&m_waits};
    if (!wait.isTaken()) {
        return ExchangeOutcome{QVariantMap{}, QString{},
                               QStringLiteral("too many callbacks are already being "
                                              "exchanged"), QString{}};
    }

    if (!isRemote()) {
        const OAuthBackend::ExchangeResult result{
            m_backend->exchange(state, code, redirectUri, presentedBinding)};
        return ExchangeOutcome{result.identity, result.tokenKey, result.error, result.context};
    }
    if (!m_remote) {
        return ExchangeOutcome{QVariantMap{}, QString{},
                               QStringLiteral("auth entity not connected"), QString{}};
    }

    const QString requestId{randomToken()};
    const AwaitScope awaited{&m_awaited, requestId};
    const auto forget{qScopeGuard([this, requestId]() { m_exchangeResults.remove(requestId); })};
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
                              Q_ARG(QString, redirectUri),
                              Q_ARG(QString, presentedBinding));
    {
        // The loop serves other callers while it spins. See SynQt::TraceScope on detaching.
        const SynQt::TraceScope untraced{SynQt::TraceContext{}};
        loop.exec();
    }

    if (!m_exchangeResults.contains(requestId)) {
        return ExchangeOutcome{QVariantMap{}, QString{},
                               QStringLiteral("auth entity timed out"), QString{}};
    }
    ExchangeOutcome outcome{m_exchangeResults.take(requestId)};
    // The tokens are held on the auth entity under the state key until the session exists.
    outcome.tokenKey = state;
    return outcome;
}

void IdentityProvider::holdClaim(const QString &code, const QByteArray &sessionId,
                                 const QString &challenge)
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    if (!isRemote()) {
        m_claims.hold(code, sessionId, challenge, now);
        return;
    }
    if (m_remote) {
        // Fire and forget: the claim is stored before the loopback redirect naming it
        // leaves this process.
        QMetaObject::invokeMethod(m_remote, "holdClaim", Q_ARG(QString, code),
                                  Q_ARG(QString, QString::fromLatin1(sessionId)),
                                  Q_ARG(QString, challenge));
    }
}

QByteArray IdentityProvider::takeClaim(const QString &code, const QString &verifier)
{
    if (!isRemote()) {
        return m_claims.take(code, verifier, QDateTime::currentMSecsSinceEpoch(),
                             claimTtlMsFrom(m_config.claimTtlSeconds));
    }
    if (!m_remote) {
        return {};
    }
    const WaitScope wait{&m_waits};
    if (!wait.isTaken()) {
        return {};
    }

    // The same bounded nested loop as begin/exchange: the handler is synchronous and the
    // answer is a correlated signal.
    const QString requestId{randomToken()};
    const AwaitScope awaited{&m_awaited, requestId};
    const auto forget{qScopeGuard([this, requestId]() { m_claimResults.remove(requestId); })};
    QEventLoop loop;
    connect(this, &IdentityProvider::claimArrived, &loop, [&loop, requestId](const QString &id) {
        if (id == requestId) {
            loop.quit();
        }
    });
    QTimer::singleShot(kRemoteTimeoutMs, &loop, &QEventLoop::quit);
    QMetaObject::invokeMethod(m_remote, "takeClaim", Q_ARG(QString, requestId),
                              Q_ARG(QString, code), Q_ARG(QString, verifier));
    {
        // The loop serves other callers while it spins. See SynQt::TraceScope on detaching.
        const SynQt::TraceScope untraced{SynQt::TraceContext{}};
        loop.exec();
    }
    return m_claimResults.take(requestId);
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
    // The back-reference goes; the family stays. An expired session is what the device
    // credential exists for: the next launch redeems it for a new session.
    if (m_devices) {
        m_devices->unbindSession(sessionId);
    }
    if (m_backend) {
        m_backend->releaseTokens(QString::fromLatin1(sessionId));
    } else {
        releaseRemoteTokens(sessionId);
    }
}

void IdentityProvider::followRotation(const QByteArray &from, const QByteArray &to)
{
    if (from.isEmpty() || to.isEmpty() || from == to) {
        return;
    }
    if (m_backend) {
        m_backend->rekeyTokens(QString::fromLatin1(from), QString::fromLatin1(to));
    } else {
        // provider_entity mode: the tokens are on the auth entity under the session id this
        // edge sent. `bindSession`, the same call the callback makes, moves them to the new
        // id as rekeyTokens does here.
        bindRemoteSession(QString::fromLatin1(from), to);
    }
    if (m_devices) {
        const QString family{m_devices->familyOf(from)};
        if (!family.isEmpty()) {
            m_devices->unbindSession(from);
            m_devices->bindSession(to, family);
        }
    }
}

QVariantMap IdentityProvider::tokensForSession(const QByteArray &sessionId) const
{
    if (m_backend) {
        return m_backend->tokens(QString::fromLatin1(sessionId));
    }
    return {};  // provider_entity mode: tokens live only on the auth entity
}

qint64 IdentityProvider::overVisitorLimit(QHash<QString, RateWindow> &table,
                                          const QHttpServerRequest &request, int limit)
{
    // The visitor address, not the peer: behind a balancer the peer is one address for
    // everybody.
    const QString visitor{m_clientAddress
                              ? m_clientAddress->resolve(request.remoteAddress(),
                                                         request.value("X-Forwarded-For"))
                              : request.remoteAddress().toString()};
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};

    // Enforce the table ceiling first, as in webedge.cpp's sign-in gate: a reference from
    // operator[] does not survive a prune (QHash::erase moves later entries), and only
    // expired windows are dropped. A table still full after that refuses.
    constexpr int kMaxRateEntries{4096};
    if (pruneRateWindows(table, now, kVisitorWindowMs, kMaxRateEntries)) {
        return kVisitorWindowMs;
    }

    RateWindow &window{table[visitor]};
    if (now - window.startedMs > kVisitorWindowMs) {
        window.startedMs = now;
        window.count = 0;
    }
    if (++window.count > limit) {
        return std::max<qint64>(1, window.startedMs + kVisitorWindowMs - now);
    }
    return 0;
}

QHttpServerResponse IdentityProvider::handleLogin(const QHttpServerRequest &request)
{
    // Each login started holds a pending flow on the engine for up to five minutes, and the
    // engine drops its oldest one when full. The window keeps any one address from churning
    // that table fast enough to drop another visitor's login before it returns.
    if (const qint64 wait{overVisitorLimit(m_loginRate, request, MaxLoginsPerVisitorMinute)};
        wait > 0) {
        return tooManyRequests(wait);
    }
    expireClaims();
    const QUrlQuery query{request.url().query()};
    QString providerName{query.queryItemValue(QStringLiteral("provider"))};
    if (providerName.isEmpty() && !m_config.providers.isEmpty()) {
        providerName = m_config.providers.first().name;
    }

    // The desktop half, decided before anything goes to the provider. A login asking for a
    // loopback answer that does not fully qualify is refused, not downgraded to the browser
    // flow, which would sign someone in while the app still waits and set a cookie in a
    // browser that is not the app.
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

    // Bind this login to the browser that started it: a random value set as a cookie now
    // and required on the callback (against login CSRF and fixation). It goes to the
    // identity engine with the state, so any edge process can answer the callback. See
    // IdentityProvider::LoginContext.
    const QString csrfToken{randomToken()};
    LoginContext context;
    context.returnUrl = returnUrl;
    context.returnState = returnState;
    context.returnChallenge = returnChallenge;

    const BeginOutcome begin{beginLogin(providerName, csrfToken, context.toJson())};
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

    return redirectTo(begin.authorizeUrl, {buildStateCookie(csrfToken.toUtf8(), false)});
}

QHttpServerResponse IdentityProvider::handleCallback(const QHttpServerRequest &request)
{
    const QUrlQuery query{request.url().query()};
    const QString code{query.queryItemValue(QStringLiteral("code"))};
    const QString state{query.queryItemValue(QStringLiteral("state"))};

    // The state and binding checks run in the identity engine, which holds the state, the
    // CSRF binding and the desktop context together. It refuses a mismatched binding before
    // spending the code and consumes the record either way, so any edge process can answer
    // the callback and the record is single-use across all of them.
    //
    // A state alone is not enough: an attacker can give a victim a valid state together
    // with the attacker's own authorization code.
    const QByteArray presentedCsrf{cookieValue(request.value("Cookie"), kOauthStateCookie)};
    const ExchangeOutcome exchange{exchangeCode(state, code,
                                                QString::fromUtf8(presentedCsrf))};
    const LoginContext context{LoginContext::fromJson(exchange.context)};

    // These two refuse the request rather than report a failed exchange, and keep the
    // browser flow's status codes.
    if (exchange.error == QLatin1String("invalid or expired state")
        || exchange.error == QLatin1String("login session mismatch")) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   exchange.error.toUtf8(),
                                   QHttpServerResponse::StatusCode::BadRequest};
    }

    if (exchange.identity.isEmpty()) {
        if (context.isDesktop()) {
            // Tell the waiting desktop client it failed, so it does not wait for the
            // timeout.
            return loopbackRedirect(context, QString{}, QStringLiteral("access_denied"));
        }
        return redirectTo(m_config.appRoute, {buildStateCookie(QByteArray{}, true)});
    }

    // A login that cannot be given a declared scope fails closed. A default such as `return
    // QStringLiteral("user")` would let a failing hook hand out an authenticated scope, and
    // a typo would give a session a scope no check can satisfy.
    QString scopeError;
    const QString scope{mapScope(exchange.identity, &scopeError)};
    if (scope.isEmpty()) {
        qWarning("SynQt: refusing a login the identity mapping hook could not place: %s",
                 qPrintable(scopeError));
        // The exchange already happened, so the provider tokens wait under the state key
        // for a session that will not exist. Release them, or they stay for the life of the
        // process and the sweep spends their refresh token.
        if (m_backend) {
            m_backend->releaseTokens(exchange.tokenKey);
        } else {
            releaseRemoteTokens(exchange.tokenKey.toLatin1());
        }
        if (context.isDesktop()) {
            return loopbackRedirect(context, QString{}, QStringLiteral("access_denied"));
        }
        return redirectTo(m_config.appRoute, {buildStateCookie(QByteArray{}, true)});
    }
    const QByteArray sessionId{m_sessions->createSession(scope, exchange.identity)};
    if (sessionId.isEmpty()) {
        // The session table is full with nothing to drop. The login succeeded but there is
        // no session to give; release the tokens under the state key, as for a refused
        // hook.
        qWarning("SynQt: refusing a login because no session can be issued right now");
        if (m_backend) {
            m_backend->releaseTokens(exchange.tokenKey);
        } else {
            releaseRemoteTokens(exchange.tokenKey.toLatin1());
        }
        if (context.isDesktop()) {
            return loopbackRedirect(context, QString{},
                                    QStringLiteral("temporarily_unavailable"));
        }
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("no session can be issued right now"),
                                   QHttpServerResponse::StatusCode::ServiceUnavailable};
    }

    // Move the tokens under the session id so refresh finds them, on the edge (in-process)
    // or the auth entity (provider_entity).
    if (isRemote()) {
        bindRemoteSession(exchange.tokenKey, sessionId);
    } else {
        m_backend->rekeyTokens(exchange.tokenKey, QString::fromLatin1(sessionId));
    }

    if (context.isDesktop()) {
        // A desktop login ends without a cookie: the system browser is not the app, and a
        // session left there would never end. The loopback carries a code that stands for
        // the session for one minute, exchangeable once, by whoever holds the verifier.
        const QString claimCode{randomToken()};
        holdClaim(claimCode, sessionId, context.returnChallenge);
        return loopbackRedirect(context, claimCode, QString{});
    }

    // Set the session cookie and clear the now-consumed login-state cookie.
    return redirectTo(m_config.appRoute,
                      {buildCookie(sessionId), buildStateCookie(QByteArray{}, true)});
}

QHttpServerResponse IdentityProvider::loopbackRedirect(const LoginContext &context,
                                                       const QString &code,
                                                       const QString &error) const
{
    // Checked again here after the check at login. The value has left this process as the
    // `context` the identity engine keeps with the state (in provider_entity mode, a JSON
    // round trip to the auth entity). The check costs one URL parse per desktop sign-in and
    // does not depend on every hop in between staying correct.
    QUrl target{context.returnUrl, QUrl::StrictMode};
    if (!isLoopbackReturn(target)) {
        // No fallback redirect. The app waiting on its loopback listener times out and
        // reports the failure.
        qWarning("SynQt: refusing to complete a desktop login whose return URL is not a "
                 "loopback address; nothing was handed back");
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("invalid return"),
                                   QHttpServerResponse::StatusCode::BadRequest};
    }
    QUrlQuery query;
    if (!code.isEmpty()) {
        query.addQueryItem(QStringLiteral("code"), code);
    }
    if (!error.isEmpty()) {
        query.addQueryItem(QStringLiteral("error"), error);
    }
    query.addQueryItem(QStringLiteral("state"), context.returnState);
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
    // No browser uses this route: the browser flow ends with a cookie. Refusing any request
    // with an Origin keeps page script out entirely.
    if (!request.value("Origin").isEmpty()) {
        return notFound();
    }

    // A code and a verifier are 64 hex characters each. Longer bodies are refused before
    // decoding. (What QHttpServer buffered before this is its own limit.)
    constexpr qsizetype kMaxClaimBody{1024};
    if (request.body().size() > kMaxClaimBody) {
        return notFound();
    }
    const QUrlQuery body{QString::fromUtf8(request.body())};
    const QString code{body.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded)};
    const QString verifier{body.queryItemValue(QStringLiteral("verifier"),
                                               QUrl::FullyDecoded)};
    // One answer for every failure (unknown, expired, spent, wrong verifier), and the code
    // is spent either way. Only the record's location differs between in-process identity
    // and the auth entity, so any replica can honour the claim.
    const QByteArray claimedSession{takeClaim(code, verifier)};
    if (claimedSession.isEmpty()) {
        return notFound();
    }

    // Enrolment, when the client asked for it and the project persists sessions. Here,
    // because this is where the session has been proven to belong to the caller; a separate
    // endpoint would be a second way in.
    DeviceRegistry::Credential credential;
    const bool wantsDevice{body.queryItemValue(QStringLiteral("device"))
                           == QLatin1String("1")};
    if (wantsDevice && m_devices) {
        const SessionRecord *record{m_sessions->lookup(claimedSession)};
        const QString sub{record ? record->identity.value(QStringLiteral("sub")).toString()
                                 : QString{}};
        if (record && !sub.isEmpty()) {
            // The level the client reports for its store. Below the configured floor this
            // returns nothing and the client keeps the claimed session without storing
            // anything.
            const DeviceBinding binding{deviceBindingFromName(
                body.queryItemValue(QStringLiteral("binding"), QUrl::FullyDecoded))};
            credential = m_devices->enrol(sub, record->identity, m_edgeOrigin, binding,
                                          body.queryItemValue(QStringLiteral("label"),
                                                              QUrl::FullyDecoded));
            bindFamily(claimedSession, credential.family);
        }
    }
    return sessionAnswer(claimedSession, credential.family, credential.secret,
                         credential.expiresMs);
}

QHttpServerResponse IdentityProvider::handleDevice(const QHttpServerRequest &request)
{
    if (!m_config.allowDesktopLogin || m_devices == nullptr) {
        return notFound();
    }
    // As on the claim route: the browser flow never comes here, so any request with an
    // Origin is refused.
    if (!request.value("Origin").isEmpty()) {
        return notFound();
    }
    constexpr qsizetype kMaxDeviceBody{1024};
    if (request.body().size() > kMaxDeviceBody) {
        return notFound();
    }

    // Per-address fixed window, limiting the cost of a guess: a 256-bit secret is not
    // brute-forced, but no one should buy a database read per packet.
    constexpr int kMaxAttemptsPerWindow{30};
    if (const qint64 wait{overVisitorLimit(m_deviceRate, request, kMaxAttemptsPerWindow)};
        wait > 0) {
        return tooManyRequests(wait);
    }

    // Checked before the credential is spent: redeeming rotates it, and a rotation that is
    // never returned leaves the client unable to sign in. Like the rate limit, it says
    // nothing about the credential and tells the client to wait.
    if (!m_sessions->hasRoom()) {
        return tooManyRequests(kVisitorWindowMs);
    }

    const QUrlQuery body{QString::fromUtf8(request.body())};
    const QString family{body.queryItemValue(QStringLiteral("device_id"), QUrl::FullyDecoded)};
    QByteArray secret{
        body.queryItemValue(QStringLiteral("device_secret"), QUrl::FullyDecoded).toUtf8()};
    const DeviceRegistry::Redemption redemption{m_devices->redeem(family, secret, m_edgeOrigin)};
    secret.fill('\0');
    secret.clear();
    if (!redemption.ok) {
        // One answer for unknown, expired, revoked, reused and wrong, so a stolen file
        // reveals nothing.
        return notFound();
    }

    // The scope is derived again from the identity stored at enrolment, so a user demoted
    // yesterday does not keep the old scope for the rest of the credential's life.
    QString scopeError;
    const QString scope{mapScope(redemption.identity, &scopeError)};
    if (scope.isEmpty()) {
        // The same answer as every other refusal here; this is a project error, and the log
        // reports the difference.
        qWarning("SynQt: refusing a device redemption the identity mapping hook could not "
                 "place: %s", qPrintable(scopeError));
        return notFound();
    }
    const QByteArray sessionId{m_sessions->createSession(scope, redemption.identity)};
    if (sessionId.isEmpty()) {
        // hasRoom() just said yes and nothing runs in between, so this is not reached.
        // Refused rather than answering with an empty session.
        return tooManyRequests(kVisitorWindowMs);
    }
    bindFamily(sessionId, redemption.next.family);
    return sessionAnswer(sessionId, redemption.next.family, redemption.next.secret,
                         redemption.next.expiresMs);
}

QHttpServerResponse IdentityProvider::sessionAnswer(const QByteArray &sessionId,
                                                    const QString &family,
                                                    const QByteArray &secret, qint64 expiresMs)
{
    QJsonObject answer;
    answer.insert(QStringLiteral("session"), QString::fromLatin1(sessionId));
    answer.insert(QStringLiteral("cookie_name"), m_cookie.name);
    if (!family.isEmpty() && !secret.isEmpty()) {
        answer.insert(QStringLiteral("device_id"), family);
        answer.insert(QStringLiteral("device_secret"), QString::fromLatin1(secret));
        const qint64 remaining{expiresMs - QDateTime::currentMSecsSinceEpoch()};
        answer.insert(QStringLiteral("expires_in"),
                      static_cast<double>(qMax(qint64{0}, remaining / 1000)));
    }
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

void IdentityProvider::bindFamily(const QByteArray &sessionId, const QString &family)
{
    if (family.isEmpty() || m_devices == nullptr) {
        return;
    }
    m_devices->bindSession(sessionId, family);
}

void IdentityProvider::onReuseDetected(const QString &family)
{
    // Two copies of one credential are in use and there is no telling which is the visitor,
    // so every session the family opened is revoked. The visitor signs in again.
    qWarning("SynQt: a retired device credential was presented past its overlap window, so "
             "the device and every session it opened have been revoked. If this was not a "
             "theft it was a client that could not store what it was given.");
    if (m_devices == nullptr) {
        return;
    }
    const QStringList sessions{m_devices->sessionsOfFamily(family)};
    for (const QString &sessionKey : sessions) {
        m_devices->unbindSessionKey(sessionKey);
        m_sessions->revokeByKey(sessionKey);
    }
}

QHttpServerResponse IdentityProvider::handleLogout(const QHttpServerRequest &request)
{
    // Signing out changes state, and this route is a GET (`Session.logout()` navigates or
    // fetches it), so it is a CSRF target: another site could end a visitor's session and
    // device credential. SameSite=Lax does not prevent a top-level navigation, and under
    // `split_origin` the cookie is SameSite=None.
    //
    // The browser sets `Sec-Fetch-Site`: `same-origin` for the app's own navigation,
    // `cross-site` for another site's. A non-browser caller (the desktop client) sends none
    // and is unaffected. `same-site` is refused only under the same-origin model; under
    // `split_origin` the app is a sibling of the edge and its sign-out arrives as
    // `same-site`. `cross-site` is always refused.
    const QByteArray site{request.value("Sec-Fetch-Site")};
    if (site == "cross-site" || (site == "same-site" && !m_cookie.sameSiteNone)) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("sign out from the application"),
                                   QHttpServerResponse::StatusCode::Forbidden};
    }

    const QByteArray sessionId{cookieValue(request.value("Cookie"), m_cookie.name.toUtf8())};
    if (!sessionId.isEmpty()) {
        // Signing out also ends the device credential: a logout that leaves a redeemable
        // credential on disk is worse than none. The family is read from what this edge
        // recorded when the session was minted, never from the request.
        if (m_devices) {
            const QString family{m_devices->familyOf(sessionId)};
            m_devices->unbindSession(sessionId);
            if (!family.isEmpty()) {
                m_devices->forget(family);
            }
        }
        m_sessions->revoke(sessionId);
        if (m_backend) {
            m_backend->releaseTokens(QString::fromLatin1(sessionId));
        } else {
            releaseRemoteTokens(sessionId);
        }
    }
    // Expire the cookie.
    QByteArray expired{m_cookie.name.toUtf8() + "=; HttpOnly; Path=/; Max-Age=0"};
    return redirectTo(m_config.appRoute, {expired});
}

void IdentityProvider::setScopeOrder(const QStringList &scopeOrder)
{
    m_scopeOrder = scopeOrder;
}

QString IdentityProvider::mapScope(const QVariantMap &identity, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return QString{};
    };

    if (!m_mapping) {
        return fail(QStringLiteral("the project declares no identity mapping hook"));
    }

    const QMetaObject *meta{m_mapping->metaObject()};
    const int methodIndex{meta->indexOfMethod("scopeFor(QVariant)")};
    if (methodIndex < 0) {
        return fail(QStringLiteral("the mapping hook has no scopeFor(identity)"));
    }

    // Two shapes, because the return annotation is part of the metaobject signature:
    // `function scopeFor(identity): int` returns int, an unannotated one returns QVariant,
    // and invoking with the wrong one fails with "return type mismatch". The scaffold and
    // the docs use the annotated form; the other is accepted too.
    const QMetaMethod method{meta->method(methodIndex)};
    bool isNumber{false};
    int index{-1};
    if (method.returnMetaType() == QMetaType::fromType<int>()) {
        isNumber = method.invoke(m_mapping, Qt::DirectConnection, Q_RETURN_ARG(int, index),
                                 Q_ARG(QVariant, QVariant{identity}));
        if (!isNumber) {
            return fail(QStringLiteral("the mapping hook's scopeFor(identity) could not be "
                                       "called"));
        }
    } else {
        QVariant result;
        if (!method.invoke(m_mapping, Qt::DirectConnection, Q_RETURN_ARG(QVariant, result),
                           Q_ARG(QVariant, QVariant{identity}))) {
            return fail(QStringLiteral("the mapping hook's scopeFor(identity) could not be "
                                       "called"));
        }
        // The hook returns a member of the generated Scope enum, whose value is the scope's
        // index in scopes.order (synqt.scopegen writes both). A bounds check is all that is
        // needed.
        index = result.toInt(&isNumber);
        if (!isNumber) {
            return fail(QStringLiteral("the mapping hook returned '%1', which is not a "
                                       "Scope member")
                            .arg(result.toString()));
        }
    }
    if (index < 0 || index >= static_cast<int>(m_scopeOrder.size())) {
        return fail(QStringLiteral("the mapping hook returned %1, which is not one of the "
                                   "%2 scopes this project declares")
                        .arg(index).arg(m_scopeOrder.size()));
    }
    return m_scopeOrder.at(index);
}

QByteArray IdentityProvider::buildStateCookie(const QByteArray &value, bool expire) const
{
    // SameSite=Lax: the cookie is sent on the top-level GET back from the provider, but not
    // on cross-site subrequests.
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
        // No `Partitioned`: this cookie is set on the callback, a top-level navigation onto
        // the edge, so a partitioned cookie would be filed under the edge's partition,
        // unreadable by the client site (tests/split-origin). That holds until the callback
        // returns the session through the client context instead.
        cookie += "; SameSite=None; Secure";
    } else {
        cookie += "; SameSite=Lax";
        if (m_cookie.secure) {
            cookie += "; Secure";
        }
    }
    return cookie;
}

QString IdentityProvider::LoginContext::toJson() const
{
    if (!isDesktop()) {
        return QString{};  // a browser login has nothing to carry. Do not send "{}" for it
    }
    QJsonObject object;
    object.insert(QStringLiteral("returnUrl"), returnUrl);
    object.insert(QStringLiteral("returnState"), returnState);
    object.insert(QStringLiteral("returnChallenge"), returnChallenge);
    return QString::fromUtf8(QJsonDocument{object}.toJson(QJsonDocument::Compact));
}

IdentityProvider::LoginContext IdentityProvider::LoginContext::fromJson(const QString &json)
{
    const QJsonObject object{QJsonDocument::fromJson(json.toUtf8()).object()};
    LoginContext context;
    context.returnUrl = object.value(QStringLiteral("returnUrl")).toString();
    context.returnState = object.value(QStringLiteral("returnState")).toString();
    context.returnChallenge = object.value(QStringLiteral("returnChallenge")).toString();
    return context;
}

void IdentityProvider::expireClaims()
{
    // Swept on entry to the login route and the claim route, so an uncollected code never
    // outlives its minute. An expiring claim takes nothing with it; its session follows the
    // session manager's own rules.
    //
    // In provider_entity mode this table is empty and the auth entity sweeps its own, with
    // the same clamp (SynQt::claimTtlMsFrom).
    m_claims.expire(QDateTime::currentMSecsSinceEpoch(),
                    claimTtlMsFrom(m_config.claimTtlSeconds));
}

} // namespace SynQt
