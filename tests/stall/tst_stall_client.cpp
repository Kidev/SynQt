// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The stall storefront, the client half: an edge-delivered page reaches the router the way
// it does in a shipped client. tst_stall fetches pages by calling the Pages replica itself;
// this drives the production path instead. The edge pushes its route table, the client's
// router learns a route it never compiled, asks for the page through the Pages consumer
// facade, and the answer comes back through the promise bridge into a component the router
// can show. The same page is refused, not shown, by a client whose palette does not declare
// everything it imports.

#include "pages_consumer.h"  // synqtRegisterPagesConsumers()
#include "pages_replica.h"   // synqtRegisterPagesReplicas()

#include "router.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTest>

#include <tuple>

using namespace SynQt;

class TestStallClient : public QObject
{
    Q_OBJECT

private:
    QProcess m_edge;
    quint16 m_port{0};
    QByteArray m_token;

    SynClientConfig clientConfig(const QStringList &palette) const
    {
        SynClientConfig config;
        config.edgeUrl = QUrl{QStringLiteral("wss://127.0.0.1:%1/sync").arg(m_port)};
        config.connectPoints = {{QStringLiteral("Pages"), QStringLiteral("Pages")}};
        config.pinnedCaCertPath = QStringLiteral(STALL_CERT_DIR "/ca.crt");
        config.sessionCookie = QByteArrayLiteral("synqt_session=") + m_token;
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user")};
        config.reconnectBaseMs = 200;
        config.remotePalette = palette;
        return config;
    }

    /// Where the router settles for \a path. It asks again while the route is still unknown,
    /// because the route table arrives from the edge a moment after the link does.
    static Router::PageStatus settle(Router *router, const QString &path)
    {
        router->go(path);
        // Whether it settled is the status below: Loading or NotFound if it never did.
        std::ignore = QTest::qWaitFor([router, path]() {
            if (router->pageStatus() == Router::NotFound) {
                router->go(path);
            }
            return router->pageStatus() != Router::Loading
                   && router->pageStatus() != Router::NotFound;
        }, 10000);
        return router->pageStatus();
    }

private slots:
    void initTestCase()
    {
        synqtRegisterPagesConsumers();
        synqtRegisterPagesReplicas();
        m_edge.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        m_edge.start(QStringLiteral(STALL_EDGE_BINARY), {});
        QVERIFY2(m_edge.waitForStarted(5000), qPrintable(m_edge.errorString()));
        while (m_port == 0 || m_token.isEmpty()) {
            QVERIFY2(m_edge.canReadLine() || m_edge.waitForReadyRead(10000),
                     "the edge never said where it listens");
            const QList<QByteArray> line{m_edge.readLine().trimmed().split(' ')};
            if (line.value(0) == "port") {
                m_port = static_cast<quint16>(line.value(1).toUInt());
            } else if (line.value(0) == "token") {
                m_token = line.value(1);
            }
        }
    }

    void cleanupTestCase()
    {
        m_edge.kill();
        m_edge.waitForFinished(5000);
    }

    void aPageTheClientNeverCompiledReachesTheRouter()
    {
        QQmlEngine engine;
        SynClient client{clientConfig({QStringLiteral("QtQuick"),
                                       QStringLiteral("QtQuick.Layouts")}),
                         &engine};
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"), 10000);

        QCOMPARE(settle(client.router(), QStringLiteral("/deal-of-the-day")), Router::Ready);
        // Handed over while its imports still compile, as a Loader expects to be given it.
        QQmlComponent *page{client.router()->pageComponent()};
        QVERIFY(page != nullptr);
        QTRY_VERIFY2(!page->isLoading(), "the delivered page never finished compiling");
        QVERIFY2(page->isReady(), qPrintable(page->errorString()));
    }

    void aPageImportingOutsideThePaletteIsRefused()
    {
        QQmlEngine engine;
        SynClient client{clientConfig({QStringLiteral("QtQuick")}), &engine};
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"), 10000);

        // Refused for the palette and for nothing else: a client that refused every page
        // would pass a check on the status alone.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{QStringLiteral(
            "refusing delivered page /deal-of-the-day: module not in the palette: "
            "QtQuick.Layouts")});
        QCOMPARE(settle(client.router(), QStringLiteral("/deal-of-the-day")), Router::Error);
        QVERIFY(client.router()->pageComponent() == nullptr);
    }
};

QTEST_MAIN(TestStallClient)
#include "tst_stall_client.moc"
