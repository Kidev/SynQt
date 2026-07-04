// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What the framework's choke points record. The pipeline tests next door prove that an
// event handed to the tracer comes out the other end; these prove that the events are
// handed over at all, from a real web edge and a real session store.
//
// Both halves at every gate. A gate watched only through its refusals looks healthy while
// it refuses everybody; the accepted case is what shows the gate still opens.

#include "actingfor.h"
#include "caller.h"
#include "promise.h"
#include "sessionmanager.h"
#include "tracer.h"
#include "tracescope.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "hall_sourcehelper.h"
#include "ledger_sourcehelper.h"

#include <QJSEngine>
#include <QJSValue>
#include <QMutex>
#include <QMutexLocker>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QScopeGuard>
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

/// The trace a continuation ran in, and what an outbound call from it would have carried.
///
/// Handed to a JS handler as an object it can call, because a promise's handler is a JS
/// value and the only way to see what the framework put back around it is to look from
/// inside.
class Probe : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE void note()
    {
        outbound = ActingFor::current();
        trace(Category::Application, Severity::Info, QStringLiteral("continued"));
        ++ran;
    }

    QVariantMap outbound;
    int ran{0};
};

/// What an owner's QML does inside a slot, written where the test can watch it: it says
/// something, it looks at what a call it makes would carry, and it chains work onto an
/// answer that will come later. The generated body finds a slot of this name beyond the
/// helper's own methods, exactly as it finds the QML function.
class Ledger : public LedgerSourceHelper
{
    Q_OBJECT

public:
    QVariantMap outbound;
    Probe probe;
    Promise *pending{nullptr};
    QJSEngine *engine{nullptr};

    using LedgerSourceHelper::post;

public slots:
    void post(QVariant item)
    {
        Q_UNUSED(item)
        trace(Category::Application, Severity::Info, QStringLiteral("posting"));
        outbound = ActingFor::current();
        if (engine != nullptr) {
            // A call whose answer arrives in a later turn, as every call over a link does.
            pending = new Promise{QRemoteObjectPendingCall{}, engine, this};
            const QJSValue factory{engine->evaluate(QStringLiteral(
                "(function (probe) { return function () { probe.note(); }; })"))};
            // `newQObject` hands the engine JavaScriptOwnership, and the engine deletes
            // what it owns once no script references it and it has no QObject parent.
            // This one is a member subobject with neither, so the collector, or the
            // engine's own destructor, would call delete on a pointer that was never
            // new'd. Said here rather than given a parent, because what is wrong is the
            // ownership and not the shape of the test.
            QJSEngine::setObjectOwnership(&probe, QJSEngine::CppOwnership);
            pending->catchError(factory.call(QJSValueList{engine->newQObject(&probe)}));
        }
    }
};

/// Same, for the contract the browser reaches. The first hop, where a trace begins.
class Hall : public HallSourceHelper
{
    Q_OBJECT

public:
    QVariantMap outbound;

    using HallSourceHelper::enter;

public slots:
    void enter(QVariant name)
    {
        Q_UNUSED(name)
        outbound = ActingFor::current();
    }
};

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
        // that session can be read out of, and docs/security.md promises it never gets there.
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

        // 1. An open slot, answered. Nothing implements it in QML here, which is an owner
        //    that did not implement it. The call still crossed, and still counts.
        //
        // The name is searched for in the whole rendered record, which carries a random
        // trace id and a random span id, so it must be a string no hex id can contain: 'g'
        // and 'r' are not hex digits.
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

    /// One click, one trace, and it starts at the edge.
    ///
    /// The browser's call is where the story begins. Nothing upstream of it names a trace
    /// (a visitor cannot), so the span the edge opens is the root, and it has to be the
    /// root of what the edge's own QML then does, or a click reaches the console as one
    /// trace per entity it touched. The call the edge makes on a service carries that
    /// span, as the parent of whatever the service records.
    void theEdgeStartsTheTraceItsOutboundCallCarries()
    {
        Recorded recorded;
        QObject owner;
        SessionManager sessions{QStringLiteral("anonymous"), 60};
        const QByteArray token{sessions.createSession(QStringLiteral("user"))};
        Hall hall;
        Caller *caller{Caller::forUser(QStringLiteral("Hall"), &sessions, token, &hall, &owner)};
        hall.synqtSetCaller(caller);

        hall.enter(QStringLiteral("ada"));

        const QList<TraceEvent> calls{recorded.withMessage(QStringLiteral("enter"))};
        QCOMPARE(calls.size(), 1);
        QVERIFY(calls.first().parentSpanId.isEmpty());
        QCOMPARE(hall.outbound.value(QStringLiteral("traceId")).toString(),
                 calls.first().traceId);
        QCOMPARE(hall.outbound.value(QStringLiteral("spanId")).toString(),
                 calls.first().spanId);
        // The session went with it, and the browser's credential did not.
        QCOMPARE(hall.outbound.value(QStringLiteral("key")).toString(),
                 SessionManager::keyFor(token));
        QVERIFY(!hall.outbound.values().contains(QVariant{QString::fromUtf8(token)}));

        // And the thread is left in nothing. A call that ended is not the trace of the
        // next thing this entity does.
        QVERIFY(!TraceScope::current().isValid());
    }

    /// A service continues the trace that arrived with the call, on a reused Caller.
    ///
    /// The parent span travels in the session map, so it is known only once the map has
    /// been taken. A span opened before that hangs off whatever the previous call left on
    /// the Caller, which is one Caller per link and answers every call the edge relays.
    /// Two calls from two clicks, and the second must not be filed under the first.
    void aServiceContinuesTheTraceEachCallArrivedWith()
    {
        Recorded recorded;
        QObject owner;
        Ledger ledger;
        Caller *caller{Caller::forEntity(QStringLiteral("Ledger"), QStringLiteral("web"), true,
                                         &ledger, &owner)};
        ledger.synqtSetCaller(caller);

        const TraceContext firstClick{
            Tracer::instance()->startSpan(TraceContext{}, QStringLiteral("add"))};
        const TraceContext secondClick{
            Tracer::instance()->startSpan(TraceContext{}, QStringLiteral("add"))};
        auto sessionIn = [](const TraceContext &click) {
            QVariantMap session;
            session.insert(QStringLiteral("key"), QStringLiteral("k"));
            session.insert(QStringLiteral("scope"), QStringLiteral("user"));
            session.insert(QStringLiteral("traceId"), click.traceId);
            session.insert(QStringLiteral("spanId"), click.spanId);
            return session;
        };

        ledger.post(sessionIn(firstClick), QStringLiteral("bread"));
        ledger.post(sessionIn(secondClick), QStringLiteral("milk"));

        const QList<TraceEvent> calls{recorded.withMessage(QStringLiteral("post"))};
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls.first().traceId, firstClick.traceId);
        QCOMPARE(calls.first().parentSpanId, firstClick.spanId);
        QCOMPARE(calls.last().traceId, secondClick.traceId);
        QCOMPARE(calls.last().parentSpanId, secondClick.spanId);

        // What the service passes further on is its own span, under the click's.
        QCOMPARE(ledger.outbound.value(QStringLiteral("traceId")).toString(),
                 secondClick.traceId);
        QCOMPARE(ledger.outbound.value(QStringLiteral("spanId")).toString(),
                 calls.last().spanId);
    }

    /// What an entity says while answering a call is part of the call.
    ///
    /// `Log.info` in a slot, a provider's query, a gate's refusal. Each is a record, and
    /// a record with no trace on it is a line in a log file, findable only by reading
    /// around it. One inside a span belongs to the span.
    void aRecordWrittenInsideACallBelongsToItsTrace()
    {
        Recorded recorded;
        QObject owner;
        Ledger ledger;
        Caller *caller{Caller::forEntity(QStringLiteral("Ledger"), QStringLiteral("web"), true,
                                         &ledger, &owner)};
        ledger.synqtSetCaller(caller);

        ledger.post(QVariantMap{}, QStringLiteral("bread"));

        const QList<TraceEvent> calls{recorded.withMessage(QStringLiteral("post"))};
        QCOMPARE(calls.size(), 1);
        const QList<TraceEvent> said{recorded.withMessage(QStringLiteral("posting"))};
        QCOMPARE(said.size(), 1);
        QCOMPARE(said.first().traceId, calls.first().traceId);
        QCOMPARE(said.first().spanId, calls.first().spanId);
        // A record has no duration and closes nothing.
        QCOMPARE(said.first().durationUs, static_cast<qint64>(-1));
        QVERIFY(said.first().parentSpanId.isEmpty());
    }

    /// A shared entity's two spans nest, rather than reading as the same call twice.
    ///
    /// A shared Source answers everybody, and the per-caller mirror in front of it hands
    /// the call over. Both run the generated body, so one call over the link is two spans.
    /// That is accurate, but only if the second is inside the first. Parented off what
    /// arrived on the wire they are siblings with the same name and the same arguments,
    /// which in the console is one call rendered as two, and an operator counting calls on
    /// a busy edge counts double.
    ///
    /// The rule that decides it: the span a call belongs to is the work enclosing it on
    /// this thread when there is any, and what the wire said only when there is not. A call
    /// arriving over a link encloses nothing (every bounded wait detaches, and a slot must
    /// not spin a loop at all), so such a call reads the wire.
    void aSharedEntityHandsTheCallOnInsideItsOwnSpan()
    {
        Recorded recorded;
        QObject owner;
        Ledger mirror;
        Ledger shared;
        Caller *mirrorCaller{Caller::forEntity(QStringLiteral("Ledger"),
                                               QStringLiteral("web"), true, &mirror, &owner)};
        Caller *sharedCaller{Caller::forEntity(QStringLiteral("Ledger"),
                                               QStringLiteral("web"), true, &shared, &owner)};
        mirror.synqtSetCaller(mirrorCaller);
        shared.synqtSetCaller(sharedCaller);
        mirror.synqtMirror(&shared);

        const TraceContext click{
            Tracer::instance()->startSpan(TraceContext{}, QStringLiteral("add"))};
        QVariantMap session;
        session.insert(QStringLiteral("key"), QStringLiteral("k"));
        click.writeTo(session);
        mirror.post(session, QStringLiteral("bread"));

        const QList<TraceEvent> calls{recorded.withMessage(QStringLiteral("post"))};
        QCOMPARE(calls.size(), 2);
        // The inner span closes first, so the shared Source's is the one recorded first.
        const TraceEvent &inner{calls.first()};
        const TraceEvent &outer{calls.last()};
        QCOMPARE(inner.traceId, click.traceId);
        QCOMPARE(outer.traceId, click.traceId);
        QCOMPARE(outer.parentSpanId, click.spanId);
        QCOMPARE(inner.parentSpanId, outer.spanId);
    }

    /// Turning ordinary calls down must not take the refusals' story with them.
    ///
    /// `monitoring.levels.call: warning` is the documented way to keep what went wrong and
    /// drop the chatter, and a busy system runs it. If the span at the head of a chain were
    /// opened only for a call that is recorded, the edge would open none under that
    /// setting, the click would name no trace, and every entity downstream would start its
    /// own: the refusal the operator kept would carry a trace identifier that leads only to
    /// itself.
    ///
    /// Two hops, both turned down, and the second one refuses.
    void aRefusedCallIsInTheClicksTraceWhenOrdinaryCallsAreNotKept()
    {
        Recorded recorded;
        Tracer::instance()->setLevel(Category::Call, Severity::Warning);
        const auto restore{qScopeGuard([]() {
            Tracer::instance()->setLevel(Category::Call, Severity::Trace);
        })};

        QObject owner;
        SessionManager sessions{QStringLiteral("anonymous"), 60};
        const QByteArray token{sessions.createSession(QStringLiteral("user"))};
        Hall hall;
        Caller *edgeCaller{
            Caller::forUser(QStringLiteral("Hall"), &sessions, token, &hall, &owner)};
        hall.synqtSetCaller(edgeCaller);

        // The first hop. An ordinary call, which at this level is not recorded at all.
        hall.enter(QStringLiteral("ada"));
        QVERIFY(recorded.withMessage(QStringLiteral("enter")).isEmpty());

        // It still has to name the story, because the next hop can only continue one that
        // exists, so a span is minted here even though the call is not recorded.
        const QVariantMap onward{hall.outbound};
        QVERIFY2(TraceContext::isTraceId(onward.value(QStringLiteral("traceId")).toString()),
                 "the click named no trace, so nothing downstream can be found by it");
        QVERIFY(TraceContext::isSpanId(onward.value(QStringLiteral("spanId")).toString()));

        // The second hop, reached with what the first one sent, refuses. The session it is
        // acting for holds `user` and the member wants `moderator`.
        Ledger ledger;
        Caller *serviceCaller{Caller::forEntity(QStringLiteral("Ledger"),
                                                QStringLiteral("web"), true, &ledger, &owner)};
        ledger.synqtSetCaller(serviceCaller);
        ledger.audit(onward, QStringLiteral("bread"));

        const QList<TraceEvent> refusals{recorded.withMessage(QStringLiteral("audit"))};
        QCOMPARE(refusals.size(), 1);
        QVERIFY(!refusals.first().ok);
        QCOMPARE(refusals.first().attributes.value(QStringLiteral("refusedBy")).toString(),
                 QStringLiteral("scope"));
        QCOMPARE(refusals.first().traceId,
                 onward.value(QStringLiteral("traceId")).toString());
        QCOMPARE(refusals.first().parentSpanId,
                 onward.value(QStringLiteral("spanId")).toString());
    }

    /// The answer to a call arrives turns later, and what runs then is still the click.
    ///
    /// `Db.read().then(rows => Cache.put(rows))` is the ordinary shape of an edge slot,
    /// and the second call is made when the slot and its span are long closed and the
    /// thread is in nothing. The continuation runs in the trace of the call that made the
    /// promise, or every click's second hop starts a trace of its own.
    void aContinuationRunsInTheTraceOfTheCallThatMadeIt()
    {
        Recorded recorded;
        QQmlEngine engine;
        QObject owner;
        Ledger ledger;
        ledger.engine = &engine;
        Caller *caller{Caller::forEntity(QStringLiteral("Ledger"), QStringLiteral("web"), true,
                                         &ledger, &owner)};
        ledger.synqtSetCaller(caller);

        ledger.post(QVariantMap{}, QStringLiteral("bread"));
        QVERIFY(ledger.pending != nullptr);
        QVERIFY(!TraceScope::current().isValid());

        // The answer comes back, in a turn of its own.
        ledger.pending->abandon(QStringLiteral("the link went away"));
        QTRY_COMPARE(ledger.probe.ran, 1);

        const QList<TraceEvent> calls{recorded.withMessage(QStringLiteral("post"))};
        QCOMPARE(calls.size(), 1);
        QCOMPARE(ledger.probe.outbound.value(QStringLiteral("traceId")).toString(),
                 calls.first().traceId);
        QCOMPARE(ledger.probe.outbound.value(QStringLiteral("spanId")).toString(),
                 calls.first().spanId);
        // The session does not follow. A continuation acts for nobody.
        QVERIFY(!ledger.probe.outbound.contains(QStringLiteral("key")));
        const QList<TraceEvent> said{recorded.withMessage(QStringLiteral("continued"))};
        QCOMPARE(said.size(), 1);
        QCOMPARE(said.first().traceId, calls.first().traceId);
        // And the handler left the thread as it found it.
        QVERIFY(!TraceScope::current().isValid());
    }

    /// The gate has a budget, because the answer behind it is expensive to give.
    ///
    /// A sign-in derives PBKDF2 at the operator store's round count, hundreds of
    /// milliseconds by design, on the edge's own event loop, and the route is open to
    /// anybody who can reach the port. Unbudgeted, an unauthenticated POST is both a
    /// password guess and the cheapest way to stop the edge answering anybody else: a
    /// handful a second keeps the loop busy.
    ///
    /// The refusal is not the gate's own "no". It is decided before the credential is read,
    /// so it says nothing about it, and it carries Retry-After so a legitimate client can
    /// wait rather than read it as a wrong password.
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

        // The decisive half: the derivation stopped being reached. Forty attempts, and the
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
        const QByteArray rightPassword{
            QByteArrayLiteral("name=ada&password=correct%20horse%20battery")};
        QNetworkReply *yes{postForm(manager, edge.httpOrigin() + config.signInPath, cookie,
                                    rightPassword)};
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
