// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What the framework's choke points record. The pipeline tests next door prove
// that an event handed to the tracer comes out the other end. These prove that the events
// are handed over at all, from a real web edge and a real session store rather than from a
// call written for the occasion.
//
// Both halves at every gate. A gate watched only through its refusals looks
// perfectly healthy while it is refusing everybody, which is how `identity.required`
// refused every visitor of every SynQt application for months without one test going red
// (tests/m5-webedge). The accepted case is what says the gate still opens.

#include "caller.h"
#include "sessionmanager.h"
#include "tracer.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "hall_sourcehelper.h"

#include <QMutex>
#include <QMutexLocker>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTest>
#include <QWebSocket>

using namespace SynQt;

namespace {

QSslConfiguration insecureClientConfig()
{
    QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);  // self-signed test cert
    return configuration;
}

WebEdgeConfig makeConfig()
{
    WebEdgeConfig config;
    config.bundleDir = QStringLiteral(MONITOR_SRCDIR "/bundle");
    config.host = QStringLiteral("127.0.0.1");
    config.port = 0;  // OS-assigned
    config.certFile = QStringLiteral(MONITOR_CERT_DIR "/server.crt");
    config.keyFile = QStringLiteral(MONITOR_CERT_DIR "/server.key");
    config.handshakeTimeoutMs = 2000;
    config.maxMessageBytes = 4096;
    return config;
}

/// Everything the process tracer recorded while this was alive, in order.
///
/// The tracer is the process-wide one, because the instrumented call sites reach it
/// through `Tracer::instance()`, exactly as they do in a running entity, so a harness that
/// handed them a tracer of its own would be testing a path no application takes.
class Recorded
{
public:
    Recorded()
    {
        Tracer::instance()->setEntity(QStringLiteral("web"));
        Tracer::instance()->setEnabled(true);
        Tracer::instance()->setBatch(1, 20);
        Tracer::instance()->setSink([this](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&m_mutex};
            m_events.append(batch);
        });
    }

    ~Recorded()
    {
        Tracer::instance()->setSink(Tracer::Sink{});
        Tracer::instance()->setEnabled(false);
    }

    QList<TraceEvent> events()
    {
        Tracer::instance()->flush();
        QMutexLocker locker{&m_mutex};
        return m_events;
    }

    QList<TraceEvent> withMessage(const QString &message)
    {
        QList<TraceEvent> matching;
        const QList<TraceEvent> all{events()};
        for (const TraceEvent &event : all) {
            if (event.message == message) {
                matching.append(event);
            }
        }
        return matching;
    }

private:
    QMutex m_mutex;
    QList<TraceEvent> m_events;
};

QNetworkReply *httpGet(QNetworkAccessManager &manager, const QString &url)
{
    QNetworkRequest request{QUrl{url}};
    request.setSslConfiguration(insecureClientConfig());
    QNetworkReply *reply{manager.get(request)};
    QSignalSpy finished{reply, &QNetworkReply::finished};
    if (!finished.wait(10000)) {
        return nullptr;
    }
    return reply;
}

QNetworkReply *postForm(QNetworkAccessManager &manager, const QString &url,
                        const QByteArray &cookie, const QByteArray &body)
{
    QNetworkRequest request{QUrl{url}};
    request.setSslConfiguration(insecureClientConfig());
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QByteArrayLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Cookie", cookie);
    QNetworkReply *reply{manager.post(request, body)};
    QSignalSpy finished{reply, &QNetworkReply::finished};
    if (!finished.wait(10000)) {
        return nullptr;
    }
    return reply;
}

QByteArray sessionCookie(QNetworkReply *reply)
{
    const QByteArray raw{reply->rawHeader("Set-Cookie")};
    return raw.left(raw.indexOf(';'));
}

} // namespace

class TestInstrumentation : public QObject
{
    Q_OBJECT

private slots:
    void anUpgradeFromADisallowedOriginIsRecordedAsARefusal()
    {
        Recorded recorded;
        QQmlEngine engine;
        WebEdge edge{makeConfig(), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkAccessManager manager;
        QNetworkReply *reply{httpGet(manager, edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();
        QVERIFY(!cookie.isEmpty());

        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        QSignalSpy connectedSpy{&socket, &QWebSocket::connected};

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", "https://evil.example");
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        QTRY_VERIFY(!recorded.withMessage(QStringLiteral("upgrade refused")).isEmpty());
        const TraceEvent event{recorded.withMessage(QStringLiteral("upgrade refused")).first()};
        QCOMPARE(event.category, Category::Authorization);
        QCOMPARE(event.severity, Severity::Warning);
        QCOMPARE(event.entity, QStringLiteral("web"));
        QVERIFY2(event.attributes.value(QStringLiteral("reason")).toString()
                     .contains(QStringLiteral("origin not allowed")),
                 "the refusal has to say which check refused it, or it is not a record");
        QCOMPARE(connectedSpy.count(), 0);  // refused before a socket exists

        // And the credential is not in it. A monitor holding a session cookie is a place
        // that session can be read out of. Docs/security.md promises it never gets there.
        for (const TraceEvent &recordedEvent : recorded.events()) {
            const QString rendered{QString::fromUtf8(
                QJsonDocument::fromVariant(recordedEvent.toVariant()).toJson())};
            QVERIFY2(!rendered.contains(QString::fromUtf8(cookie.mid(cookie.indexOf('=') + 1))),
                     qPrintable(QStringLiteral("a session credential reached the monitor: ")
                                + rendered));
        }
    }

    void anAuthorizedUpgradeIsRecordedAsAnAcceptance()
    {
        Recorded recorded;
        QQmlEngine engine;
        WebEdge edge{makeConfig(), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkAccessManager manager;
        QNetworkReply *reply{httpGet(manager, edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();
        QVERIFY(!cookie.isEmpty());

        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        QSignalSpy connectedSpy{&socket, &QWebSocket::connected};

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        QTRY_VERIFY(connectedSpy.count() == 1);
        QTRY_VERIFY(!recorded.withMessage(QStringLiteral("upgrade accepted")).isEmpty());
        const TraceEvent event{recorded.withMessage(QStringLiteral("upgrade accepted")).first()};
        QCOMPARE(event.category, Category::Transport);
        QVERIFY(event.ok);
        QVERIFY(recorded.withMessage(QStringLiteral("upgrade refused")).isEmpty());
    }

    void theSessionLifecycleIsRecordedByItsHandleAndNotItsCredential()
    {
        Recorded recorded;
        SessionManager sessions{QStringLiteral("anonymous"), 60};

        const QByteArray anonymous{sessions.createSession()};
        const QByteArray elevated{sessions.setScope(
            anonymous, QStringLiteral("user"),
            QVariantMap{{QStringLiteral("sub"), QStringLiteral("1")}})};
        QVERIFY(!elevated.isEmpty());
        sessions.revoke(elevated);

        const QList<TraceEvent> created{recorded.withMessage(QStringLiteral("session created"))};
        QCOMPARE(created.size(), 1);
        QCOMPARE(created.first().attributes.value(QStringLiteral("scope")).toString(),
                 QStringLiteral("anonymous"));
        QCOMPARE(created.first().attributes.value(QStringLiteral("session")).toString(),
                 SessionManager::keyFor(anonymous));

        const QList<TraceEvent> changed{
            recorded.withMessage(QStringLiteral("session scope changed"))};
        QCOMPARE(changed.size(), 1);
        // Both handles, because what an operator chases is which session became which.
        QCOMPARE(changed.first().attributes.value(QStringLiteral("previousSession")).toString(),
                 SessionManager::keyFor(anonymous));
        QCOMPARE(changed.first().attributes.value(QStringLiteral("session")).toString(),
                 SessionManager::keyFor(elevated));
        QCOMPARE(changed.first().attributes.value(QStringLiteral("scope")).toString(),
                 QStringLiteral("user"));

        QCOMPARE(recorded.withMessage(QStringLiteral("session revoked")).size(), 1);

        // The handle is not the credential, and no credential is anywhere in the record.
        for (const TraceEvent &event : recorded.events()) {
            const QString rendered{QString::fromUtf8(
                QJsonDocument::fromVariant(event.toVariant()).toJson())};
            QVERIFY(!rendered.contains(QString::fromLatin1(anonymous)));
            QVERIFY(!rendered.contains(QString::fromLatin1(elevated)));
        }
    }

    void anEntityWithNoMonitorRecordsNothingAtAll()
    {
        // The default an application that never asked for monitoring runs under. It is the
        // whole reason the instrumentation can be unconditional. The call sites are there,
        // and they cost a comparison.
        Recorded recorded;
        Tracer::instance()->setEnabled(false);

        SessionManager sessions{QStringLiteral("anonymous"), 60};
        sessions.revoke(sessions.createSession());

        QVERIFY(recorded.events().isEmpty());
    }

    // The generated half. Every slot the contract compiler emits opens a span at the top
    // of its body and closes it in a destructor, so the record is written whichever way
    // the call leaves: refused by the gate, refused by a bound, or answered.
    void aSlotCrossingALinkIsRecordedByItsShapeAndNotItsArguments()
    {
        Recorded recorded;
        SessionManager sessions{QStringLiteral("anonymous"), 60};
        const QByteArray token{sessions.createSession(QStringLiteral("user"))};

        QObject owner;
        HallSourceHelper hall;
        Caller *caller{Caller::forUser(QStringLiteral("Hall"), &sessions, token, &hall,
                                       &owner)};
        caller->setScopeOrder({QStringLiteral("anonymous"), QStringLiteral("user"),
                               QStringLiteral("moderator")}, true);
        hall.synqtSetCaller(caller);

        // 1. An open slot, answered. Nothing implements it in QML here, which is exactly
        //    an owner who did not implement it. The call still crossed, and still counts.
        //
        //    The name is checked for below by searching the whole rendered record, which
        //    carries a random trace id and a random span id, so it has to be a string no
        //    hex id can contain: 'g' and 'r' are not hex digits, and "ada" (which was
        //    here) is three that are. It turned up inside a span id about once in thirty
        //    runs and failed the suite on macOS.
        hall.enter(QStringLiteral("grace"));

        // 2. The same caller at the same scope, refused by the gate.
        hall.promote(QStringLiteral("grace"));

        // 3. Refused by the bound on the argument, before the owner sees any of it.
        hall.enter(QString(64, QLatin1Char('x')));

        const QList<TraceEvent> calls{recorded.withMessage(QStringLiteral("enter"))
                                      + recorded.withMessage(QStringLiteral("promote"))};
        QCOMPARE(calls.size(), 3);
        for (const TraceEvent &event : calls) {
            QCOMPARE(event.category, Category::Call);
            QCOMPARE(event.attributes.value(QStringLiteral("contract")).toString(),
                     QStringLiteral("Hall"));
            QCOMPARE(event.attributes.value(QStringLiteral("caller")).toString(),
                     QStringLiteral("user"));
            QCOMPARE(event.attributes.value(QStringLiteral("args")).toInt(), 1);
            QVERIFY(event.durationUs >= 0);
            // The shape, never the contents. "grace" is what a person typed, and the
            // record of an operation is not where what they typed ends up.
            QVERIFY(!event.attributes.contains(QStringLiteral("name")));
            const QString rendered{QString::fromUtf8(
                QJsonDocument::fromVariant(event.toVariant()).toJson())};
            QVERIFY2(!rendered.contains(QStringLiteral("grace")),
                     qPrintable(QStringLiteral("an argument value reached the record: ")
                                + rendered));
        }

        const QList<TraceEvent> answered{recorded.withMessage(QStringLiteral("enter"))};
        QCOMPARE(answered.size(), 2);
        QVERIFY(answered.first().ok);
        QVERIFY(!answered.first().attributes.contains(QStringLiteral("refusedBy")));

        // And each refusal names which check made it, or they all look alike in the record.
        QCOMPARE(answered.last().attributes.value(QStringLiteral("refusedBy")).toString(),
                 QStringLiteral("bound"));
        const QList<TraceEvent> gated{recorded.withMessage(QStringLiteral("promote"))};
        QCOMPARE(gated.size(), 1);
        QVERIFY(!gated.first().ok);
        QCOMPARE(gated.first().severity, Severity::Warning);
        QCOMPARE(gated.first().attributes.value(QStringLiteral("refusedBy")).toString(),
                 QStringLiteral("scope"));
    }

    void aCallHangsOffTheTraceTheCallerBroughtWithIt()
    {
        Recorded recorded;
        QObject owner;
        HallSourceHelper hall;
        Caller *caller{Caller::forEntity(QStringLiteral("Hall"), QStringLiteral("web"), true,
                                         &hall, &owner)};
        const TraceContext upstream{
            Tracer::instance()->startSpan(TraceContext{}, QStringLiteral("upgrade"))};
        caller->setTraceContext(upstream);
        hall.synqtSetCaller(caller);

        hall.enter(QStringLiteral("ada"));

        const QList<TraceEvent> calls{recorded.withMessage(QStringLiteral("enter"))};
        QCOMPARE(calls.size(), 1);
        // One click, one story. The entity's own work continues the trace that reached it
        // rather than starting a second one nothing can be joined to.
        QCOMPARE(calls.first().traceId, upstream.traceId);
        QCOMPARE(calls.first().parentSpanId, upstream.spanId);
        QCOMPARE(calls.first().attributes.value(QStringLiteral("caller")).toString(),
                 QStringLiteral("entity"));
    }

    /// The gate has a budget, because the answer behind it is expensive to give.
    ///
    /// A sign-in derives PBKDF2 at the operator store's round count, on purpose
    /// hundreds of milliseconds, on the edge's own event loop, and the route is open to
    /// anybody who can reach the port. So an unauthenticated POST was both a password guess
    /// and the cheapest way there is to stop the edge answering anybody else: a handful a
    /// second is enough to keep the loop busy, and nothing counted them.
    ///
    /// The refusal is not the gate's own "no". It is decided before the
    /// credential is read, so it says nothing about it, and it carries Retry-After so a
    /// legitimate client can wait rather than read it as "this password is wrong".
    void aFloodOfSignInAttemptsIsRefusedBeforeThePasswordIsChecked()
    {
        QQmlEngine engine;
        WebEdgeConfig config{makeConfig()};
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("operator")};
        config.defaultScope = QStringLiteral("anonymous");
        config.signInPath = QStringLiteral("/monitor/signin");
        config.signInScope = QStringLiteral("operator");
        int checked{0};
        config.signIn = [&checked](const QString &name, const QString &password) {
            ++checked;
            return (name == QStringLiteral("ada"))
                    && (password == QStringLiteral("correct horse battery"));
        };
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkAccessManager manager;
        QNetworkReply *page{httpGet(manager, edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(page != nullptr);
        const QByteArray cookie{sessionCookie(page)};
        page->deleteLater();

        // Spend the window on wrong guesses, then keep going. Somewhere in here the answer
        // has to stop being 401.
        int lastStatus{0};
        QByteArray retryAfter;
        for (int attempt{0}; attempt < 40; ++attempt) {
            QNetworkReply *reply{postForm(manager, edge.httpOrigin() + config.signInPath,
                                          cookie, QByteArrayLiteral("name=ada&password=no"))};
            QVERIFY(reply != nullptr);
            lastStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            retryAfter = reply->rawHeader("Retry-After");
            reply->deleteLater();
            if (lastStatus == 429) {
                break;
            }
        }
        QCOMPARE(lastStatus, 429);
        QVERIFY2(!retryAfter.isEmpty(), "a refusal a client should wait out has to say so");

        // The decisive half. The derivation stopped being reached. Forty attempts, and the
        // gate itself was consulted for only the handful the budget allows.
        QVERIFY2(checked < 40,
                 qPrintable(QStringLiteral("the password was checked %1 times for 40 "
                                           "attempts, so nothing was rationed")
                                .arg(checked)));

        // And the right password is refused too while the window lasts, which is what makes
        // it a budget rather than a filter on wrong guesses.
        QNetworkReply *correct{postForm(
            manager, edge.httpOrigin() + config.signInPath, cookie,
            QByteArrayLiteral("name=ada&password=correct%20horse%20battery"))};
        QVERIFY(correct != nullptr);
        QCOMPARE(correct->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 429);
        QVERIFY(correct->rawHeader("Set-Cookie").isEmpty());
        correct->deleteLater();
    }

    // The console's gate. The monitor authenticates its own operators, because an operator
    // is not a user of the application and the application's login provider is often the
    // thing they are signing in to investigate.
    void anOperatorSignsInAndTheirSessionIsRaisedRatherThanReplaced()
    {
        QQmlEngine engine;
        WebEdgeConfig config{makeConfig()};
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("operator")};
        config.defaultScope = QStringLiteral("anonymous");
        config.signInPath = QStringLiteral("/monitor/signin");
        config.signInScope = QStringLiteral("operator");
        config.signIn = [](const QString &name, const QString &password) {
            return (name == QStringLiteral("ada"))
                    && (password == QStringLiteral("correct horse battery"));
        };
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkAccessManager manager;
        QNetworkReply *page{httpGet(manager, edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(page != nullptr);
        const QByteArray cookie{sessionCookie(page)};
        page->deleteLater();
        QVERIFY(!cookie.isEmpty());

        // The wrong password. One answer, and the session is left exactly as it was.
        QSignalSpy refused{&edge, &WebEdge::signInRefused};
        QNetworkReply *no{postForm(manager, edge.httpOrigin() + config.signInPath, cookie,
                                   QByteArrayLiteral("name=ada&password=wrong"))};
        QVERIFY(no != nullptr);
        QCOMPARE(no->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 401);
        QVERIFY(no->rawHeader("Set-Cookie").isEmpty());
        no->deleteLater();
        QCOMPARE(refused.count(), 1);

        // The right one: the session they already hold is raised rather than replaced,
        // which is what makes the delivery gate work. `setScope` rotates the credential,
        // so a fresh cookie comes back.
        QSignalSpy accepted{&edge, &WebEdge::signInAccepted};
        QNetworkReply *yes{postForm(manager, edge.httpOrigin() + config.signInPath, cookie,
                                    QByteArrayLiteral("name=ada&password=correct%20horse%20battery"))};
        QVERIFY(yes != nullptr);
        QCOMPARE(yes->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        const QByteArray raised{sessionCookie(yes)};
        yes->deleteLater();
        QVERIFY(!raised.isEmpty());
        QVERIFY2(raised != cookie, "the credential has to rotate when the scope changes");
        QCOMPARE(accepted.count(), 1);

        const QByteArray token{raised.mid(raised.indexOf('=') + 1)};
        const SessionRecord *record{edge.sessionManager()->lookup(token)};
        QVERIFY(record != nullptr);
        QCOMPARE(record->scope, QStringLiteral("operator"));

        // And the password is nowhere in what the gate said about itself.
        QCOMPARE(accepted.first().first().toString(), QStringLiteral("ada"));
    }
};

QTEST_MAIN(TestInstrumentation)
#include "tst_instrumentation.moc"
