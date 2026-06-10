// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The inbound HTTP surface (`network.inbound`), an entity serving a public API through
// routes its own QML declared on `Api`, behind the checks `ApiServer` runs first.
//
// Two halves are under test. That the surface works at all. A route matches, captures a
// placeholder, reads a JSON body, and answers with JSON. And that nothing reaches a
// handler that should not: no API key, an origin nobody allowed, a body over the limit,
// and a flood past the rate limit are each answered by the server.
//
// The rate limit brings a third question with it, since a limit per address is only as
// good as its notion of address. Whether `X-Forwarded-For` is believed, which turns on
// whether the peer that sent it is a proxy this surface was told about.

#include "api.h"
#include "apiconfig.h"
#include "apiserver.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QUrl>

#include <memory>

using namespace SynQt;

namespace {

struct Answer
{
    int status{0};
    QByteArray body;

    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

} // namespace

class TestApiInbound : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<ApiServer> m_server;
    QObject *m_gateway{nullptr};
    QNetworkAccessManager m_network;
    quint16 m_port{0};

    QUrl url(const QString &path) const
    {
        return QUrl{QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path)};
    }

    Answer send(const QString &method, const QString &path, const QByteArray &body = {},
                const QByteArray &key = QByteArrayLiteral("right-key"),
                const QByteArray &origin = {}, const QByteArray &forwardedFor = {})
    {
        QNetworkRequest request{url(path)};
        if (!key.isEmpty()) {
            request.setRawHeader(QByteArrayLiteral("X-API-Key"), key);
        }
        if (!origin.isEmpty()) {
            request.setRawHeader(QByteArrayLiteral("Origin"), origin);
        }
        if (!forwardedFor.isEmpty()) {
            request.setRawHeader(QByteArrayLiteral("X-Forwarded-For"), forwardedFor);
        }
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QByteArrayLiteral("application/json"));
        QNetworkReply *reply{method == QLatin1String("POST")
                                 ? m_network.post(request, body)
                                 : m_network.get(request)};
        QSignalSpy finished{reply, &QNetworkReply::finished};
        finished.wait(5000);
        Answer answer;
        answer.status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        answer.body = reply->readAll();
        reply->deleteLater();
        return answer;
    }

    /// A throwaway surface with a rate limit on it, and how many of a run of requests it
    /// refused.
    ///
    /// One server per case, because the limit is a property of the surface: sharing one
    /// would make the order the cases run in part of what they assert. `forwardedFor` is
    /// one entry per request, and an empty one sends no header at all.
    void countRefusals(const QStringList &trustedProxies, int perMinute,
                       const QList<QByteArray> &forwardedFor, int *refused)
    {
        QQmlEngine engine;
        ApiConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.anonymous = true;  // the key is not what is under test here
        config.ratePerMinutePerIp = perMinute;
        config.trustedProxies = trustedProxies;
        ApiServer server{config, &engine};
        engine.rootContext()->setContextProperty(QStringLiteral("Api"), server.api());
        QJSValue handler{engine.evaluate(QStringLiteral("(function(r){ return {ok: true}; })"))};
        server.api()->get(QStringLiteral("/ping"), handler);
        QVERIFY2(server.start(), qPrintable(server.errorString()));

        const quint16 port{server.serverPort()};
        *refused = 0;
        for (const QByteArray &claimed : forwardedFor) {
            QNetworkRequest request{
                QUrl{QStringLiteral("http://127.0.0.1:%1/ping").arg(port)}};
            if (!claimed.isEmpty()) {
                request.setRawHeader(QByteArrayLiteral("X-Forwarded-For"), claimed);
            }
            QNetworkReply *reply{m_network.get(request)};
            QSignalSpy finished{reply, &QNetworkReply::finished};
            finished.wait(5000);
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 429) {
                *refused += 1;
            }
            reply->deleteLater();
        }
    }

private slots:
    void initTestCase()
    {
        m_engine = std::make_unique<QQmlEngine>();

        ApiConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;  // OS-assigned
        config.apiKeys = {QByteArrayLiteral("right-key")};
        config.allowedOrigins = {QStringLiteral("https://partner.example")};
        config.maxBodyBytes = 512;
        config.ratePerMinutePerIp = 0;  // off unless a case turns it on
        config.replyTimeoutMs = 300;    // short, so the deadline case is not a slow test

        m_server = std::make_unique<ApiServer>(config, m_engine.get());
        // `Api` on the root context before the entity's own file is created, exactly as
        // the generated main does it. The singleton declares its routes as it is built.
        m_engine->rootContext()->setContextProperty(QStringLiteral("Api"), m_server->api());

        // Registered and then asked for, which is exactly what the generated main does
        // with an entity's own singleton. Registering alone would leave it uncreated until
        // something used it, and nothing does until a request arrives, so its routes would
        // not exist when the first caller knocked.
        qmlRegisterSingletonType(
            QUrl::fromLocalFile(QStringLiteral(APIINBOUND_SRCDIR "/gateway/Gateway.qml")),
            "SynQt", 1, 0, "Gateway");
        m_gateway = m_engine->singletonInstance<QObject *>("SynQt", "Gateway");
        QVERIFY2(m_gateway, "the gateway entity singleton was not created");

        QVERIFY2(m_server->start(), qPrintable(m_server->errorString()));
        m_port = m_server->serverPort();
        QVERIFY(m_port != 0);
    }

    void cleanupTestCase()
    {
        // The singleton belongs to the engine, so the engine retires it. The server goes
        // first because its routes hold JS values from that engine.
        m_server.reset();
        m_gateway = nullptr;
        m_engine.reset();
    }

    void aRouteAnswersAndAPlaceholderIsCaptured()
    {
        const Answer lots{send(QStringLiteral("GET"), QStringLiteral("/lots"))};
        QCOMPARE(lots.status, 200);
        QCOMPARE(lots.json().value(QStringLiteral("lots")).toArray().size(), 2);

        const Answer one{send(QStringLiteral("GET"), QStringLiteral("/lots/42"))};
        QCOMPARE(one.status, 200);
        QCOMPARE(one.json().value(QStringLiteral("id")).toString(), QStringLiteral("42"));
    }

    void theMoreLiteralRouteWinsWhicheverWasDeclaredFirst()
    {
        // /lots/open is declared after /lots/<id> in Gateway.qml, and still takes it.
        const Answer open{send(QStringLiteral("GET"), QStringLiteral("/lots/open"))};
        QCOMPARE(open.status, 200);
        QVERIFY(open.json().value(QStringLiteral("open")).toBool());
    }

    void aJsonBodyAndTheQueryStringReachTheHandler()
    {
        const Answer created{send(QStringLiteral("POST"), QStringLiteral("/lots?dry=yes"),
                                  QByteArrayLiteral(R"({"name":"chair"})"))};
        QCOMPARE(created.status, 200);
        QCOMPARE(created.json().value(QStringLiteral("created")).toString(),
                 QStringLiteral("chair"));
        QCOMPARE(created.json().value(QStringLiteral("query")).toString(),
                 QStringLiteral("yes"));
    }

    void theHandlerDecidesItsOwnRefusal()
    {
        const Answer refused{send(QStringLiteral("POST"), QStringLiteral("/lots"),
                                  QByteArrayLiteral("{}"))};
        QCOMPARE(refused.status, 422);
        QCOMPARE(refused.json().value(QStringLiteral("error")).toString(),
                 QStringLiteral("a lot needs a name"));
    }

    void aHandlerMayAnswerOnALaterTurn()
    {
        // The shape `Api`'s own documentation is written in. The handler takes the request,
        // returns nothing, and replies once something it was waiting for arrives. The
        // connection is held open for it rather than answered with a 500.
        const Answer slow{send(QStringLiteral("GET"), QStringLiteral("/slow"))};
        QCOMPARE(slow.status, 200);
        QCOMPARE(slow.json().value(QStringLiteral("late")).toBool(), true);
    }

    void aHandlerThatNeverAnswersIsA504AndNotAHeldSocket()
    {
        QSignalSpy refused{m_server.get(), &ApiServer::requestRefused};
        const Answer silent{send(QStringLiteral("GET"), QStringLiteral("/silent"))};
        QCOMPARE(silent.status, 504);
        QCOMPARE(refused.count(), 1);
        QVERIFY(refused.at(0).at(0).toString().contains(QStringLiteral("did not answer")));
    }

    void anUnroutedPathIs404()
    {
        QCOMPARE(send(QStringLiteral("GET"), QStringLiteral("/nothing")).status, 404);
    }

    void aHandlerThatThrowsIs500AndTheServerKeepsServing()
    {
        QCOMPARE(send(QStringLiteral("GET"), QStringLiteral("/broken")).status, 500);
        // Still answering afterwards. One bad handler is not the end of the surface.
        QCOMPARE(send(QStringLiteral("GET"), QStringLiteral("/lots")).status, 200);
    }

    void noKeyAndAWrongKeyAreBothRefusedBeforeTheHandler()
    {
        QSignalSpy refused{m_server.get(), &ApiServer::requestRefused};
        QCOMPARE(send(QStringLiteral("GET"), QStringLiteral("/lots"),
                      {}, QByteArray{}).status, 401);
        QCOMPARE(send(QStringLiteral("GET"), QStringLiteral("/lots"),
                      {}, QByteArrayLiteral("wrong-key")).status, 401);
        QCOMPARE(refused.count(), 2);

        // And a wrong key on a path that does not exist is still 401, not 404: whether a
        // route exists is not something an unauthenticated caller gets to learn.
        QCOMPARE(send(QStringLiteral("GET"), QStringLiteral("/nothing"),
                      {}, QByteArrayLiteral("wrong-key")).status, 401);
    }

    void anOriginNobodyAllowedIsRefusedAndAnAllowedOneIsNot()
    {
        QCOMPARE(send(QStringLiteral("GET"), QStringLiteral("/lots"), {},
                      QByteArrayLiteral("right-key"),
                      QByteArrayLiteral("https://evil.example")).status, 403);
        QCOMPARE(send(QStringLiteral("GET"), QStringLiteral("/lots"), {},
                      QByteArrayLiteral("right-key"),
                      QByteArrayLiteral("https://partner.example")).status, 200);
    }

    /// A browser is what `allowed_origins` exists for, and a browser never sends the key
    /// on its first request. A cross-origin call carrying `X-API-Key` is preflighted with
    /// an OPTIONS that carries no key, and the real request is sent only if the preflight
    /// is answered with the origin and the header allowed. Until this was written the
    /// preflight was refused with a 401 and no CORS header, so a browser could never reach
    /// a surface that had named its origin, which is the one thing the key exists to do.
    void aNamedOriginIsAnsweredThePreflightABrowserSendsFirst()
    {
        const auto preflight{[&](const QByteArray &origin) {
            QNetworkRequest request{url(QStringLiteral("/lots"))};
            request.setRawHeader(QByteArrayLiteral("Origin"), origin);
            request.setRawHeader(QByteArrayLiteral("Access-Control-Request-Method"),
                                 QByteArrayLiteral("GET"));
            request.setRawHeader(QByteArrayLiteral("Access-Control-Request-Headers"),
                                 QByteArrayLiteral("x-api-key, content-type"));
            QNetworkReply *reply{m_network.sendCustomRequest(request, QByteArrayLiteral("OPTIONS"))};
            QSignalSpy finished{reply, &QNetworkReply::finished};
            finished.wait(5000);
            return reply;
        }};

        // The named origin. The preflight is answered, and answered with exactly what the
        // browser asked about, so the real request follows.
        QNetworkReply *allowed{preflight(QByteArrayLiteral("https://partner.example"))};
        QCOMPARE(allowed->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 204);
        QCOMPARE(allowed->rawHeader("Access-Control-Allow-Origin"),
                 QByteArrayLiteral("https://partner.example"));
        QVERIFY2(allowed->rawHeader("Access-Control-Allow-Headers").toLower().contains("x-api-key"),
                 allowed->rawHeader("Access-Control-Allow-Headers").constData());
        QVERIFY(allowed->rawHeader("Access-Control-Allow-Methods").contains("GET"));
        QVERIFY(allowed->rawHeader("Vary").contains("Origin"));
        allowed->deleteLater();

        // And the real request, which does carry the key, comes back with the header the
        // browser needs to hand the answer to the page.
        QNetworkRequest real{url(QStringLiteral("/lots"))};
        real.setRawHeader(QByteArrayLiteral("Origin"), QByteArrayLiteral("https://partner.example"));
        real.setRawHeader(QByteArrayLiteral("X-API-Key"), QByteArrayLiteral("right-key"));
        QNetworkReply *reply{m_network.get(real)};
        QSignalSpy finished{reply, &QNetworkReply::finished};
        finished.wait(5000);
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QCOMPARE(reply->rawHeader("Access-Control-Allow-Origin"),
                 QByteArrayLiteral("https://partner.example"));
        reply->deleteLater();

        // An origin nobody named gets no preflight answer and no header, so a key that
        // leaked into a page there still buys nothing. Refused rather than answered: the
        // absence of Access-Control-Allow-Origin is what stops the browser.
        QNetworkReply *refused{preflight(QByteArrayLiteral("https://evil.example"))};
        QCOMPARE(refused->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 403);
        QVERIFY(refused->rawHeader("Access-Control-Allow-Origin").isEmpty());
        refused->deleteLater();
    }

    void aBodyOverTheLimitIsRefusedBeforeTheHandler()
    {
        const QByteArray big{QByteArrayLiteral(R"({"name":")")
                             + QByteArray(600, 'x') + QByteArrayLiteral(R"("})")};
        QCOMPARE(send(QStringLiteral("POST"), QStringLiteral("/lots"), big).status, 413);
    }

    /// The declared body limit is the transport's limit, not a check made after the fact.
    ///
    /// `refuse()` reads `request.body()`, which is a body QHttpServer has already read into
    /// memory. On its own it bounds what a handler is handed and not what the process
    /// allocates. Qt's own ceiling is 32 MiB, so before this an unauthenticated caller could
    /// spend 32 MiB per connection whatever `network.inbound.max_body_bytes` said, and could
    /// keep doing it. Four megabytes here because that is the shape of the gap: far past the
    /// declared limit, far short of Qt's default, so only a limit the transport knows about
    /// refuses it.
    ///
    /// Proved by where the refusal comes from rather than by its status, which is 413 either
    /// way. A request Qt turns away never reaches ApiServer, so nothing of SynQt's has anything
    /// to say about it.
    void aBodyPastTheLimitIsRefusedByTheTransportAndNotByTheHandler()
    {
        QSignalSpy refused{m_server.get(), &ApiServer::requestRefused};
        const QByteArray huge{QByteArrayLiteral(R"({"name":")")
                              + QByteArray(4 * 1024 * 1024, 'x') + QByteArrayLiteral(R"("})")};
        // Refused, and by whichever of the two shapes a transport refusal takes: an early
        // 413, or the connection ended part way through the upload, which reaches the
        // client as no status at all. Both are the server declining to read the rest. What
        // matters is that neither of them is 200 and neither of them comes from ApiServer.
        const int status{send(QStringLiteral("POST"), QStringLiteral("/lots"), huge).status};
        QVERIFY2(status == 413 || status == 0,
                 qPrintable(QStringLiteral("a 4 MiB body was answered with %1").arg(status)));
        QVERIFY2(refused.isEmpty(),
                 "the body reached ApiServer, so the transport was still buffering it");

        // And the surface is still serving afterwards, so the refusal ends one request
        // rather than the connection's usefulness.
        QCOMPARE(send(QStringLiteral("GET"), QStringLiteral("/lots")).status, 200);
    }

    void theRateLimitAnswers429WithoutReachingAHandler()
    {
        int refused{0};
        countRefusals(QStringList{}, 3, QList<QByteArray>(5), &refused);
        QCOMPARE(refused, 2);  // three allowed in the window, two refused
    }

    /// The rate limit counts one address, and the question here is which one.
    ///
    /// A surface that trusts nobody counts the peer, and `X-Forwarded-For` is then a field
    /// the client filled in. Believing it would hand every caller a way to pick a fresh
    /// budget per request, which removes the limit rather than loosening it: five requests
    /// under a limit of three would all be served.
    void aForgedForwardedHeaderDoesNotBuyAFreshBudget()
    {
        int refused{0};
        countRefusals(QStringList{}, 3,
                      {QByteArrayLiteral("9.9.9.1"), QByteArrayLiteral("9.9.9.2"),
                       QByteArrayLiteral("9.9.9.3"), QByteArrayLiteral("9.9.9.4"),
                       QByteArrayLiteral("9.9.9.5")},
                      &refused);
        QCOMPARE(refused, 2);
    }

    /// And the other half, without which the first is a broken feature. With a proxy
    /// named, the surface counts what that proxy forwarded, so two callers behind one
    /// balancer have a budget each instead of sharing the balancer's.
    ///
    /// Six requests at a limit of two. Three from each caller, so each spends its two and
    /// is refused once. Counting the peer would refuse four of the six.
    void aTrustedProxyGivesEachCallerItsOwnBudget()
    {
        int refused{0};
        countRefusals({QStringLiteral("127.0.0.1")}, 2,
                      {QByteArrayLiteral("203.0.113.1"), QByteArrayLiteral("203.0.113.2"),
                       QByteArrayLiteral("203.0.113.1"), QByteArrayLiteral("203.0.113.2"),
                       QByteArrayLiteral("203.0.113.1"), QByteArrayLiteral("203.0.113.2")},
                      &refused);
        QCOMPARE(refused, 2);
    }

    /// What the handler is handed, which has to be the same answer the limit counted or
    /// the two would disagree about who is calling. This surface trusts nobody, so a
    /// forged header changes nothing. The peer is the client.
    void theHandlerIsHandedTheAddressTheLimitCounts()
    {
        const Answer mine{send(QStringLiteral("GET"), QStringLiteral("/whoami"), {},
                               QByteArrayLiteral("right-key"), {},
                               QByteArrayLiteral("9.9.9.9"))};
        QCOMPARE(mine.status, 200);
        QCOMPARE(mine.json().value(QStringLiteral("client")).toString(),
                 QStringLiteral("127.0.0.1"));
    }

    /// The same property on a surface that does name a proxy, where the answer is the
    /// address behind it. Declared in C++ rather than in the fixture's QML because it
    /// needs a second server with a different configuration, not a second route.
    void aTrustedProxySaysWhoIsCallingAndOnlyForTheHopsItVouchedFor()
    {
        QQmlEngine engine;
        ApiConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.anonymous = true;
        config.ratePerMinutePerIp = 0;
        config.trustedProxies = {QStringLiteral("127.0.0.1")};
        ApiServer server{config, &engine};
        engine.rootContext()->setContextProperty(QStringLiteral("Api"), server.api());
        QJSValue handler{
            engine.evaluate(QStringLiteral("(function(r){ return {client: r.client}; })"))};
        server.api()->get(QStringLiteral("/whoami"), handler);
        QVERIFY2(server.start(), qPrintable(server.errorString()));

        const auto ask{[&](const QByteArray &forwarded) {
            QNetworkRequest request{QUrl{QStringLiteral("http://127.0.0.1:%1/whoami")
                                             .arg(server.serverPort())}};
            request.setRawHeader(QByteArrayLiteral("X-Forwarded-For"), forwarded);
            QNetworkReply *reply{m_network.get(request)};
            QSignalSpy finished{reply, &QNetworkReply::finished};
            finished.wait(5000);
            const QByteArray body{reply->readAll()};
            reply->deleteLater();
            return QJsonDocument::fromJson(body).object()
                .value(QStringLiteral("client")).toString();
        }};

        QCOMPARE(ask(QByteArrayLiteral("203.0.113.7")), QStringLiteral("203.0.113.7"));

        // A balancer appends what it saw rather than replacing what was there, so the
        // entries to the left of the rightmost untrusted one are whatever the client sent.
        // Taking the leftmost would let the client name its own address.
        QCOMPARE(ask(QByteArrayLiteral("1.2.3.4, 203.0.113.7")),
                 QStringLiteral("203.0.113.7"));
    }
};

QTEST_MAIN(TestApiInbound)
#include "tst_apiinbound.moc"
