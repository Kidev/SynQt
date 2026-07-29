// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The inbound HTTP surface (`network.inbound`), an entity serving a public API through
// routes its own QML declared on `Api`, behind the checks `ApiServer` runs first.
//
// Two halves are under test: that the surface works at all (a route matches, captures a
// placeholder, reads a JSON body, and answers with JSON), and that nothing reaches a
// handler that should not: no API key, an origin nobody allowed, a body over the limit,
// and a flood past the rate limit are each answered by the server.
//
// The rate limit brings a third question with it, since a limit per address is only as
// good as its notion of address: whether `X-Forwarded-For` is believed, which turns on
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
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QStringList>
#include <QTcpSocket>
#include <QTest>
#include <QUrl>

#include <memory>
#include <vector>

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

    // The surface's own half of TLS. Named a certificate and a key, it serves over them, and
    // a caller that trusts the CA gets its answer; a caller speaking plaintext to the same
    // port gets none. Named a pair it cannot read, it refuses to start, rather than listen
    // on a port whose every handshake would fail with nothing in the log saying why.
    void inboundTlsServesOverWhatItWasNamedOrRefusesToStart()
    {
        QQmlEngine engine;
        ApiConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.anonymous = true;
        config.certFile = QStringLiteral(APIINBOUND_CERT_DIR "/server.crt");
        config.keyFile = QStringLiteral(APIINBOUND_CERT_DIR "/server.key");
        ApiServer server{config, &engine};
        server.api()->get(QStringLiteral("/ping"),
                          engine.evaluate(QStringLiteral("(function(r){ return {tls: true}; })")));
        QVERIFY2(server.start(), qPrintable(server.errorString()));

        QNetworkRequest request{
            QUrl{QStringLiteral("https://localhost:%1/ping").arg(server.serverPort())}};
        QSslConfiguration trust{QSslConfiguration::defaultConfiguration()};
        trust.setCaCertificates(
            QSslCertificate::fromPath(QStringLiteral(APIINBOUND_CERT_DIR "/ca.crt")));
        request.setSslConfiguration(trust);
        QNetworkReply *reply{m_network.get(request)};
        QSignalSpy finished{reply, &QNetworkReply::finished};
        QVERIFY(finished.wait(5000));
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object()
                     .value(QStringLiteral("tls")).toBool(), true);
        reply->deleteLater();

        QNetworkReply *plain{m_network.get(QNetworkRequest{
            QUrl{QStringLiteral("http://127.0.0.1:%1/ping").arg(server.serverPort())}})};
        QSignalSpy plainFinished{plain, &QNetworkReply::finished};
        QVERIFY(plainFinished.wait(5000));
        QVERIFY(plain->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200);
        plain->deleteLater();

        ApiConfig unreadable{config};
        unreadable.keyFile = QStringLiteral(APIINBOUND_CERT_DIR "/missing.key");
        ApiServer refused{unreadable, &engine};
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{QStringLiteral("cannot read")});
        QVERIFY(!refused.start());
        QVERIFY2(refused.errorString().contains(QStringLiteral("cannot terminate TLS")),
                 qPrintable(refused.errorString()));
    }

    // A surface with nowhere to listen is refused at start with the config key it is
    // missing, and one that does listen answers every method a route may name, PATCH
    // included, and refuses a body past its limit before a handler sees it.
    void theSurfaceNamesWhatIsMissingAndAnswersEveryMethod()
    {
        QQmlEngine engine;
        ApiConfig nowhere;
        nowhere.host.clear();
        nowhere.port = 0;
        ApiServer unplaced{nowhere, &engine};
        QVERIFY(!unplaced.start());
        QVERIFY2(unplaced.errorString().contains(QStringLiteral("network.inbound")),
                 qPrintable(unplaced.errorString()));

        ApiConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.anonymous = true;
        config.maxBodyBytes = 64;
        ApiServer server{config, &engine};
        server.api()->route(QStringLiteral("patch"), QStringLiteral("/lots/:id"),
                            engine.evaluate(QStringLiteral(
                                "(function(r){ return {patched: r.params.id, "
                                "method: r.method}; })")));
        QVERIFY2(server.start(), qPrintable(server.errorString()));

        const auto patch{[&](const QByteArray &body) {
            QNetworkRequest request{
                QUrl{QStringLiteral("http://127.0.0.1:%1/lots/3").arg(server.serverPort())}};
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QByteArrayLiteral("application/json"));
            QNetworkReply *reply{m_network.sendCustomRequest(request, "PATCH", body)};
            QSignalSpy finished{reply, &QNetworkReply::finished};
            finished.wait(5000);
            Answer answer;
            answer.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            answer.body = reply->readAll();
            reply->deleteLater();
            return answer;
        }};
        const Answer patched{patch(QByteArrayLiteral("{\"open\":false}"))};
        QCOMPARE(patched.status, 200);
        QCOMPARE(patched.json().value(QStringLiteral("patched")).toString(), QStringLiteral("3"));
        QCOMPARE(patched.json().value(QStringLiteral("method")).toString(),
                 QStringLiteral("PATCH"));

        const Answer tooLarge{patch(QByteArray(200, 'x'))};
        QCOMPARE(tooLarge.status, 413);
    }

    // What a route table accepts. Every method has its declaration and `route()` takes any,
    // upper-cased. A path that is not a route, or a handler that is not a function, is
    // refused with a warning naming it rather than stored to fail at the first request. A
    // route declared twice keeps the first handler, and one added after the server started
    // listening is announced, because nothing will route to it until the table is rebuilt.
    void theRouteTableRefusesWhatCouldNeverAnswer()
    {
        QQmlEngine engine;
        Api api{&engine};
        const QJSValue first{engine.evaluate(QStringLiteral("(function(r){ return 1; })"))};
        const QJSValue second{engine.evaluate(QStringLiteral("(function(r){ return 2; })"))};
        api.put(QStringLiteral("/lots/:id"), first);
        api.del(QStringLiteral("/lots/:id"), first);
        api.route(QStringLiteral("patch"), QStringLiteral("/lots/:id"), first);
        QStringList methods;
        for (const Api::Route &route : api.routes()) {
            methods.append(route.method);
        }
        methods.sort();
        QCOMPARE(methods, QStringList({QStringLiteral("DELETE"), QStringLiteral("PATCH"),
                                       QStringLiteral("PUT")}));

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{
            QStringLiteral("Api.get\\(\"lots\", ...\\) is not a valid route path")});
        api.get(QStringLiteral("lots"), first);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{
            QStringLiteral("Api.get\\(\"/lots\", ...\\) was given something that is not a "
                           "function")});
        api.get(QStringLiteral("/lots"), QJSValue{42});
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{
            QStringLiteral("PUT /lots/:id is declared twice; the first handler is kept")});
        api.put(QStringLiteral("/lots/:id"), second);
        QCOMPARE(api.routes().size(), 3);
        for (const Api::Route &route : api.routes()) {
            if (route.method == QLatin1String("PUT")) {
                QCOMPARE(route.handler.call({}).toInt(), 1);
            }
        }

        QSignalSpy late{&api, &Api::routeAddedLate};
        api.get(QStringLiteral("/before"), first);
        QCOMPARE(late.count(), 0);
        api.setListening(true);
        api.get(QStringLiteral("/after"), first);
        QCOMPARE(late.count(), 1);
        QCOMPARE(late.at(0).at(0).toString(), QStringLiteral("GET"));
        QCOMPARE(late.at(0).at(1).toString(), QStringLiteral("/after"));
    }

    // PUT and DELETE are answered like GET and POST, each by its own handler, and a method a
    // path does not declare is not answered by the handler of another.
    void putAndDeleteReachTheirOwnHandlers()
    {
        QQmlEngine engine;
        ApiConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.anonymous = true;
        ApiServer server{config, &engine};
        server.api()->put(QStringLiteral("/lots/:id"), engine.evaluate(QStringLiteral(
            "(function(r){ return {method: 'put', id: r.params.id}; })")));
        server.api()->del(QStringLiteral("/lots/:id"), engine.evaluate(QStringLiteral(
            "(function(r){ return {method: 'delete', id: r.params.id}; })")));
        QVERIFY2(server.start(), qPrintable(server.errorString()));

        const auto ask = [&](const QByteArray &verb) {
            QNetworkRequest request{QUrl{QStringLiteral("http://127.0.0.1:%1/lots/7")
                                             .arg(server.serverPort())}};
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QByteArrayLiteral("application/json"));
            QNetworkReply *reply{m_network.sendCustomRequest(request, verb, QByteArray{"{}"})};
            QSignalSpy finished{reply, &QNetworkReply::finished};
            finished.wait(5000);
            Answer answer;
            answer.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            answer.body = reply->readAll();
            reply->deleteLater();
            return answer;
        };
        const Answer put{ask(QByteArrayLiteral("PUT"))};
        QCOMPARE(put.status, 200);
        QCOMPARE(put.json().value(QStringLiteral("method")).toString(), QStringLiteral("put"));
        QCOMPARE(put.json().value(QStringLiteral("id")).toString(), QStringLiteral("7"));
        const Answer removed{ask(QByteArrayLiteral("DELETE"))};
        QCOMPARE(removed.status, 200);
        QCOMPARE(removed.json().value(QStringLiteral("method")).toString(),
                 QStringLiteral("delete"));
        const Answer got{ask(QByteArrayLiteral("GET"))};
        QVERIFY2(got.status == 404 || got.status == 405,
                 qPrintable(QStringLiteral("a GET nobody declared answered %1").arg(got.status)));
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

    /// A browser is what `allowed_origins` exists for, and a browser never sends the key on
    /// its first request. A cross-origin call carrying `X-API-Key` is preflighted with an
    /// OPTIONS that carries no key, and the real request is sent only if the preflight is
    /// answered with the origin and the header allowed. Refusing the preflight with a 401
    /// would keep every browser away from a surface that named its origin.
    void aNamedOriginIsAnsweredThePreflightABrowserSendsFirst()
    {
        const auto preflight{[&](const QByteArray &origin) {
            QNetworkRequest request{url(QStringLiteral("/lots"))};
            request.setRawHeader(QByteArrayLiteral("Origin"), origin);
            request.setRawHeader(QByteArrayLiteral("Access-Control-Request-Method"),
                                 QByteArrayLiteral("GET"));
            request.setRawHeader(QByteArrayLiteral("Access-Control-Request-Headers"),
                                 QByteArrayLiteral("x-api-key, content-type"));
            QNetworkReply *reply{
                m_network.sendCustomRequest(request, QByteArrayLiteral("OPTIONS"))};
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
        real.setRawHeader(QByteArrayLiteral("Origin"),
                          QByteArrayLiteral("https://partner.example"));
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
    /// `refuse()` reads `request.body()`, a body QHttpServer has already read into memory,
    /// so on its own it bounds what a handler is handed and not what the process allocates.
    /// Qt's own ceiling is 32 MiB, so an unauthenticated caller could spend 32 MiB per
    /// connection whatever `network.inbound.max_body_bytes` said. Four megabytes here: far
    /// past the declared limit and far short of Qt's default, so only a limit the transport
    /// knows about refuses it.
    ///
    /// Proved by where the refusal comes from rather than by its status, which is 413
    /// either way. A request Qt turns away never reaches ApiServer.
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

    /// A caller that opens sockets and sends nothing is seen by neither the rate limit nor
    /// the body ceiling, since both see a request, so without a socket ceiling one address
    /// could hold as many sockets as the operating system allows. Counted at accept,
    /// through Qt's own ceilings, which count correctly here because an API socket is never
    /// upgraded (the edge counts its own for that reason).
    void aPeerThatOpensSocketsAndSendsNothingIsRefusedAtTheCeiling()
    {
        QQmlEngine engine;
        ApiConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.anonymous = true;
        config.maxConnectionsPerIp = 3;
        ApiServer server{config, &engine};
        engine.rootContext()->setContextProperty(QStringLiteral("Api"), server.api());
        QJSValue handler{engine.evaluate(QStringLiteral("(function(r){ return {ok: true}; })"))};
        server.api()->get(QStringLiteral("/ping"), handler);
        QVERIFY2(server.start(), qPrintable(server.errorString()));

        // Three idle sockets from this address, which is the whole of its allowance.
        std::vector<std::unique_ptr<QTcpSocket>> held;
        for (int opened{0}; opened < 3; ++opened) {
            auto socket{std::make_unique<QTcpSocket>()};
            socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
            QVERIFY(socket->waitForConnected(3000));
            held.push_back(std::move(socket));
        }
        QTest::qWait(100);  // accepted, and counted

        // The one over the ceiling is told so and hung up on, before it has sent a byte.
        QTcpSocket extra;
        extra.connectToHost(QHostAddress::LocalHost, server.serverPort());
        QVERIFY(extra.waitForConnected(3000));
        QSignalSpy hungUp{&extra, &QTcpSocket::disconnected};
        QTRY_VERIFY_WITH_TIMEOUT(hungUp.count() >= 1 || extra.bytesAvailable() > 0, 5000);
        const QByteArray answer{extra.readAll()};
        QVERIFY2(answer.contains("429"), qPrintable(QStringLiteral("the socket over the "
                                                    "ceiling was answered %1")
                                                    .arg(QString::fromUtf8(answer))));

        // The three within it are still there, and releasing one readmits the next.
        for (const auto &socket : held) {
            QCOMPARE(socket->state(), QAbstractSocket::ConnectedState);
        }
        held.front()->abort();
        held.erase(held.begin());
        QTest::qWait(100);
        QTcpSocket readmitted;
        readmitted.connectToHost(QHostAddress::LocalHost, server.serverPort());
        QVERIFY(readmitted.waitForConnected(3000));
        readmitted.write("GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        // Spun rather than waited on, since the server answers from this same event loop.
        QTRY_VERIFY_WITH_TIMEOUT(readmitted.bytesAvailable() > 0, 5000);
        QVERIFY(readmitted.readAll().startsWith("HTTP/1.1 200"));
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
