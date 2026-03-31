// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The inbound HTTP surface (`network.inbound`): an entity serving a public API through
// routes its own QML declared on `Api`, behind the checks `ApiServer` runs first.
//
// Two halves are under test. That the surface works at all: a route matches, captures a
// placeholder, reads a JSON body, and answers with JSON. And that nothing reaches a
// handler that should not: no API key, an origin nobody allowed, a body over the limit,
// and a flood past the rate limit are each answered by the server.

#include "api.h"
#include "apiconfig.h"
#include "apiserver.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
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
                const QByteArray &origin = {})
    {
        QNetworkRequest request{url(path)};
        if (!key.isEmpty()) {
            request.setRawHeader(QByteArrayLiteral("X-API-Key"), key);
        }
        if (!origin.isEmpty()) {
            request.setRawHeader(QByteArrayLiteral("Origin"), origin);
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
        // Its own server, because the limit is a property of the surface and turning it on
        // for the cases above would make each of them count against the others.
        QQmlEngine engine;
        ApiConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.anonymous = true;  // the key is not what is under test here
        config.ratePerMinutePerIp = 3;
        ApiServer server{config, &engine};
        engine.rootContext()->setContextProperty(QStringLiteral("Api"), server.api());
        QJSValue handler{engine.evaluate(QStringLiteral("(function(r){ return {ok: true}; })"))};
        server.api()->get(QStringLiteral("/ping"), handler);
        QVERIFY2(server.start(), qPrintable(server.errorString()));

        const quint16 port{server.serverPort()};
        int limited{0};
        for (int attempt{0}; attempt < 5; ++attempt) {
            QNetworkRequest request{
                QUrl{QStringLiteral("http://127.0.0.1:%1/ping").arg(port)}};
            QNetworkReply *reply{m_network.get(request)};
            QSignalSpy finished{reply, &QNetworkReply::finished};
            finished.wait(5000);
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 429) {
                ++limited;
            }
            reply->deleteLater();
        }
        QCOMPARE(limited, 2);  // three allowed in the window, two refused
    }
};

QTEST_MAIN(TestApiInbound)
#include "tst_apiinbound.moc"
