// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The transport spike's client controller, built natively. The spike itself runs in a
// browser (tests/m0-transport), where nothing counts what a reconnect leaves behind. Every
// attempt builds a fresh node, socket and replica, so every attempt has to release them.

#include "m0controller.h"

#include "rep_spike_replica.h"

#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTest>
#include <QUrl>

class TestSpikeController : public QObject
{
    Q_OBJECT

private slots:
    void aReconnectReleasesTheReplica();
};

void TestSpikeController::aReconnectReleasesTheReplica()
{
    // A port nothing listens on, so every attempt fails and schedules the next one.
    QTcpServer reserved;
    QVERIFY(reserved.listen(QHostAddress::LocalHost));
    const quint16 port{reserved.serverPort()};
    reserved.close();

    M0Controller controller{QUrl{QStringLiteral("ws://127.0.0.1:%1").arg(port)}};
    const QPointer<SpikeSourceReplica> first{controller.findChild<SpikeSourceReplica *>()};
    QVERIFY2(first, "the replica is not owned by anything the controller tears down");

    // The first retry is half a second out. Tearing the old node down takes its replica.
    QTRY_VERIFY_WITH_TIMEOUT(first.isNull(), 5000);
    QVERIFY(controller.findChild<SpikeSourceReplica *>() != nullptr);
}

QTEST_MAIN(TestSpikeController)

#include "tst_spikecontroller.moc"
