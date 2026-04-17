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
#include <QEventLoop>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QQmlComponent>
#include <QScopeGuard>
#include <QTimer>
#include <QUrlQuery>
#include <QtQml/qqmlengine.h>

#include <utility>

// Asking the running thread how much stack it has. There is no Qt API for it, and the
// nesting bound below is only as good as this answer. Guarded so nothing but the platform
// that needs each of these ever sees it.
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

/// "You are going too fast", and not the answer above.
///
/// Everything else on these routes answers alike, so nothing learns which half
/// of a guess was right. This refusal is decided before the credential is so much as read,
/// so it says nothing about it, and it must be distinguishable. A legitimate client reading
/// "too fast" as "this credential is dead" deletes the visitor's stored sign-in over
/// somebody else behind the same address making a nuisance of themselves. `Retry-After` is
/// what is left of the window, so a client can wait it out instead of guessing.
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

/// How many answers this edge may be waiting on at once, wherever the answer comes from.
///
/// Each wait is a nested QEventLoop, which keeps serving requests while it spins, so a
/// second request that also waits nests inside the first. The routes that do this are open
/// (a GET to the login route is enough), so without a ceiling the nesting depth is whatever
/// a caller opens connections for, and the stack is what runs out. Far above the number of
/// logins any real deployment has in flight at one instant, because each of these lasts one
/// round trip (over the mesh to the auth entity, or out to the identity provider) rather than
/// a browser's whole visit to a provider.
constexpr int kMaxConcurrentWaits{64};

/// What to assume a thread's stack is when the platform will not say.
///
/// The smallest SynQt runs on rather than the roomiest. A Windows thread gets a megabyte,
/// where Linux and macOS give the main one eight.
constexpr quintptr kAssumedStackBytes{1024 * 1024};

/// How much of a thread's stack the nesting may spend. One part in four.
///
/// The count above is a policy number, and it cannot answer this on its own. What the
/// nesting spends is stack, and what a level of it costs is decided by Qt's call chain and
/// the compiler rather than by this code. Sixty-four levels fits the eight megabytes Linux and
/// macOS give the main thread and does not fit the megabyte Windows gives it: the ceiling
/// meant to stop the stack running out sat above the stack on the platform with the
/// smallest of them, and a Windows edge crashed on exactly the traffic the count was there
/// to refuse. So the stack is asked how big it is and a quarter of it is what the nesting
/// may have. Where it is roomy the count still decides, unchanged. Where it is not, this
/// does, and the refusal is the same one either way.
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

    // `this` is a local in the frame that is about to wait, so its address is where that
    // frame sits. The outermost wait records its own and reads the budget off the thread
    // it is running on. Every wait under it is that much further along the stack.
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

    // Staying signed in across relaunches. Only for a project that both asked for it and
    // builds a desktop client, because enrolment happens at the claim exchange and nothing
    // but a native client ever reaches that.
    if (m_config.device.enabled && m_config.allowDesktopLogin) {
        auto *registry{new DeviceRegistry{m_config.device, this}};
        QString error;
        if (registry->open(&error)) {
            m_devices = registry;
            connect(m_devices, &DeviceRegistry::reuseDetected,
                    this, &IdentityProvider::onReuseDetected);
        } else {
            // Loud, and then off: an edge that refused to start over an unreachable device
            // store would trade "desktop users sign in every launch" for "nobody signs in at
            // all". What must never happen quietly is the other direction, so this says
            // which store and why, and the feature is not there.
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
    // The auth entity's answers connect by name into this object's receiving slots (the Identity
    // Replica is dynamic, so string-based connect as in SessionManager::attachRemote). As
    // with the session cache, this provider must be destroyed while the Replica is still
    // alive, since the Replica frees its runtime metaobject on destruction.
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

    // Delegate to the auth entity. Invoke the slot, then wait (bounded) for the correlated
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
    loop.exec();

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
    // The ceiling covers both ways of running identity, because both of them wait inside a
    // nested event loop and the loop is what has to be counted. In provider_entity mode the
    // wait is for the auth entity's answer. In process it is `OAuthBackend::exchange`
    // spinning its own loop around the token exchange with the provider. That second one had
    // no bound at all, and it is the reachable one: the callback route is open, a state this
    // engine issued is all it takes to get past the first check, and up to
    // `kMaxPendingLogins` of those can be in flight. Callbacks arriving together then nest
    // one loop inside another until the stack, rather than any limit, decides.
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
    loop.exec();

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
        // Fire and forget. The claim is written before the loopback redirect that names it
        // leaves this process, and the client cannot present it before it has that.
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

    // The same bounded nested loop the begin/exchange pair uses, for the same reason: the
    // route handler is synchronous and the answer comes back as a correlated signal.
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
    loop.exec();
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
    // The back-reference goes, the family stays. A session running out of time is exactly
    // what the device credential is for. The next launch redeems it and gets a new session.
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
        // provider_entity mode. The tokens are the auth entity's, held under the session id
        // this edge told it about. `bindSession` is the same call the callback makes, so
        // rebinding under the new id moves them there exactly as rekeyTokens does here.
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

QHttpServerResponse IdentityProvider::handleLogin(const QHttpServerRequest &request)
{
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

    // Bind this login to the browser that started it. A random value set as a cookie now
    // and required to match on the callback (defeats login CSRF / fixation). It goes to the
    // identity engine with the state rather than into a table here, so the callback is
    // answerable by any edge process. See IdentityProvider::LoginContext.
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

    // The state and binding checks happen where the record is, which is the
    // identity engine. It holds the state, the CSRF binding and the desktop context
    // together, refuses a callback whose binding does not match BEFORE spending the code,
    // and consumes the record either way. Doing it there rather than here is what lets an
    // edge process answer a callback for a login it never saw the start of, and what keeps
    // the record single-use across every process rather than once per process.
    //
    // The login-CSRF defense is unchanged in substance. A state alone is still not enough,
    // because an attacker can hand a victim a valid state and their own authorization code.
    // What changed is only which process is holding the value it is checked against.
    const QByteArray presentedCsrf{cookieValue(request.value("Cookie"), kOauthStateCookie)};
    const ExchangeOutcome exchange{exchangeCode(state, code,
                                                QString::fromUtf8(presentedCsrf))};
    const LoginContext context{LoginContext::fromJson(exchange.context)};

    // These two are refusals of the request, not failures of the exchange, and they keep the
    // status codes the browser flow has always answered them with.
    if (exchange.error == QLatin1String("invalid or expired state")
        || exchange.error == QLatin1String("login session mismatch")) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   exchange.error.toUtf8(),
                                   QHttpServerResponse::StatusCode::BadRequest};
    }

    if (exchange.identity.isEmpty()) {
        if (context.isDesktop()) {
            // Tell the waiting desktop client it failed. Without this the app sits on its
            // loopback listener until the timeout with nothing to report, which reads to
            // the visitor as a sign-in that hung rather than one that was refused.
            return loopbackRedirect(context, QString{}, QStringLiteral("access_denied"));
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

    if (context.isDesktop()) {
        // A desktop login ends here, and not with a cookie: the system browser
        // is not the app. Leaving it signed in would put a live session in a browser the
        // visitor did not sign in with, on a machine that may not be theirs alone, and
        // nothing would ever end it. What crosses the loopback is a code that stands for the
        // session for the next minute, exchangeable once, by whoever holds the verifier.
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
    // Checked again here, and not only at login. isLoopbackReturn is the line this flow
    // turns on. Whatever passes it is where a freshly authenticated visitor's browser is
    // sent. The value arriving here has been out of this process in between, carried as
    // the `context` the identity engine keeps beside the state, which in provider_entity
    // mode means a round trip to the auth entity and back through JSON. Nothing has gone
    // wrong with that today. But a check whose correctness depends on every hop between two
    // distant points in the code is a check that stops holding the first time somebody adds
    // a hop, and this one costs a URL parse on a path that runs once per desktop sign-in.
    QUrl target{context.returnUrl, QUrl::StrictMode};
    if (!isLoopbackReturn(target)) {
        // Not a redirect to somewhere safer. There is nowhere safe to send this. The app
        // waiting on its loopback listener times out and says the sign-in failed, which is
        // the truth.
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
    // One answer for every way this can fail (unknown code, expired code, code already
    // spent, wrong verifier), and the code is spent either way. One code, one attempt.
    // Where the record lives is the only thing that differs between an edge running
    // identity in process and one asking the auth entity, which is what lets a replica
    // that never saw the login honour the claim it produced.
    const QByteArray claimedSession{takeClaim(code, verifier)};
    if (claimedSession.isEmpty()) {
        return notFound();
    }

    // Enrolment, when the client asked for it and the project persists sessions. No route of
    // its own. This is the one place where a session has been proven to belong to the
    // process asking, and adding an endpoint would be a second way to reach the same thing.
    DeviceRegistry::Credential credential;
    const bool wantsDevice{body.queryItemValue(QStringLiteral("device"))
                           == QLatin1String("1")};
    if (wantsDevice && m_devices) {
        const SessionRecord *record{m_sessions->lookup(claimedSession)};
        const QString sub{record ? record->identity.value(QStringLiteral("sub")).toString()
                                 : QString{}};
        if (record && !sub.isEmpty()) {
            // The level the client reports about its own store. Below the configured floor
            // this returns nothing, and that is not an error. The client stays signed in
            // with the session it claimed and writes nothing anywhere.
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
    // As on the claim route, and for the same reason. The browser flow ends with a cookie and
    // never comes here, so refusing anything carrying an Origin puts this endpoint out of
    // reach of page script altogether rather than relying on a secret staying unguessable.
    if (!request.value("Origin").isEmpty()) {
        return notFound();
    }
    constexpr qsizetype kMaxDeviceBody{1024};
    if (request.body().size() > kMaxDeviceBody) {
        return notFound();
    }

    // Per-address fixed window. Cheap, and it is about the cost of a guess rather than its
    // chance of succeeding. A 256-bit secret is not brute-forced, but nothing should be able
    // to buy a database read per packet.
    constexpr int kMaxAttemptsPerWindow{30};
    constexpr qint64 kWindowMs{60 * 1000};
    // The visitor's address, not the peer's. Behind a balancer the peer is one address for
    // everybody, so a window keyed on it would be a single global budget that any one
    // client can exhaust for every other client at once.
    const QString peer{m_clientAddress
                           ? m_clientAddress->resolve(request.remoteAddress(),
                                                      request.value("X-Forwarded-For"))
                           : request.remoteAddress().toString()};
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};

    // The table's own ceiling, taken first, for the two reasons webedge.cpp's sign-in gate
    // spells out: a reference from operator[] does not survive a prune (QHash::erase moves
    // the entries after the one it removes), and dropping only what has run out is what
    // keeps the ceiling from being a way to clear the count. A table that is still full of
    // live windows after that is a refusal rather than a reset.
    constexpr int kMaxRateEntries{4096};
    if (pruneRateWindows(m_deviceRate, now, kWindowMs, kMaxRateEntries)) {
        return tooManyRequests(kWindowMs);
    }

    RateWindow &window{m_deviceRate[peer]};
    if (now - window.startedMs > kWindowMs) {
        window.startedMs = now;
        window.count = 0;
    }
    if (++window.count > kMaxAttemptsPerWindow) {
        return tooManyRequests(window.startedMs + kWindowMs - now);
    }

    const QUrlQuery body{QString::fromUtf8(request.body())};
    const QString family{body.queryItemValue(QStringLiteral("device_id"), QUrl::FullyDecoded)};
    QByteArray secret{
        body.queryItemValue(QStringLiteral("device_secret"), QUrl::FullyDecoded).toUtf8()};
    const DeviceRegistry::Redemption redemption{m_devices->redeem(family, secret, m_edgeOrigin)};
    secret.fill('\0');
    secret.clear();
    if (!redemption.ok) {
        // One answer for unknown, expired, revoked, reused and wrong alike. Telling them
        // apart would tell whoever found a file on a disk which half of it still works.
        return notFound();
    }

    // The scope is re-derived here, every time, from the identity stored at enrolment. This
    // is the reason that identity is a column instead of a lookup: somebody demoted from
    // moderator yesterday must not carry moderator for the remaining 29 days of a credential
    // issued while they still were one.
    const QString scope{mapScope(redemption.identity)};
    const QByteArray sessionId{m_sessions->createSession(scope, redemption.identity)};
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
    // Two copies of one credential are in play and there is no telling which of them is the
    // visitor, so everything the family opened goes. The row is already gone, and here go the
    // sessions it minted. Somebody signs in again, which is the correct cost of the one event
    // that means a credential was copied off a machine.
    qWarning("SynQt: a retired device credential was presented past its overlap window, so "
             "the device and every session it opened have been revoked. If this was not a "
             "theft it was a client that could not store what it was given.");
    if (m_devices == nullptr) {
        return;
    }
    const QList<QByteArray> sessions{m_devices->sessionsOfFamily(family)};
    for (const QByteArray &sessionId : sessions) {
        m_devices->unbindSession(sessionId);
        m_sessions->revoke(sessionId);
    }
}

QHttpServerResponse IdentityProvider::handleLogout(const QHttpServerRequest &request)
{
    // Signing out is a state change, and this route is reached by a GET, which is what
    // `Session.logout()` does on both clients: the browser navigates to it and the desktop
    // client fetches it. That makes it a cross-site request forgery target: another site
    // need only navigate a visitor here to end their session, and with it the device
    // credential that would have kept them signed in. The cookie's SameSite=Lax does not
    // cover it (a top-level navigation is exactly what Lax still sends), and in
    // `split_origin` the cookie is SameSite=None and covers nothing at all.
    //
    // `Sec-Fetch-Site` is the header that separates the two, and it is set by the browser
    // rather than by the page. `same-origin` for the app's own navigation, `cross-site`
    // for somebody else's. A caller that is not a browser (the desktop client) sends none,
    // and is unaffected. Refused rather than answered, so nothing is ended.
    // `same-site` is refused only under the same-origin model, where the app and the edge
    // share an origin and a legitimate sign-out is always `same-origin`. Under
    // `split_origin` the app is a sibling of the edge by design, so its own sign-out
    // arrives as `same-site` and refusing it would break the one deployment that needs it;
    // there the bar for an attacker rises to controlling a sibling subdomain. `cross-site`
    // is refused either way, which is the case a stray link can reach.
    const QByteArray site{request.value("Sec-Fetch-Site")};
    if (site == "cross-site" || (site == "same-site" && !m_cookie.sameSiteNone)) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("sign out from the application"),
                                   QHttpServerResponse::StatusCode::Forbidden};
    }

    const QByteArray sessionId{cookieValue(request.value("Cookie"), m_cookie.name.toUtf8())};
    if (!sessionId.isEmpty()) {
        // Signing out ends the credential too, and it has to. A logout that leaves a
        // redeemable credential on disk is worse than no logout at all, because the visitor
        // believes it worked. This is also the only thing that ends a family early, which is
        // why it reads the family from what this edge recorded when the session was minted
        // rather than from anything the caller sent.
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
    // Swept on the way in to the two routes that can add one, so an uncollected code cannot
    // outlive its minute even on an edge nobody signs into again. A claim that expires here
    // takes nothing with it. The session it stood for is a real session, and it lives or
    // expires on the session manager's own terms.
    //
    // In provider_entity mode this table is empty and the auth entity sweeps its own, on
    // the same clamp (SynQt::claimTtlMsFrom), so a claim cannot expire on one side and not
    // the other.
    m_claims.expire(QDateTime::currentMSecsSinceEpoch(),
                    claimTtlMsFrom(m_config.claimTtlSeconds));
}

} // namespace SynQt
