// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Which address is the visitor, when the peer might be a load balancer.
//
// Every per-IP limit on the edge is only as good as its notion of "IP". Facing the
// internet directly that is the peer address. Behind a balancer the peer is the same
// address for every visitor at once, and the visitor's own address arrives in a header
// that any client can also write. So the rule is two-sided and neither half is optional:
// the header is read only from a peer the deployment named, and within it only the
// rightmost entry that is not itself a named hop is taken.

#include "clientaddress.h"

#include <QHostAddress>
#include <QStringList>
#include <QTest>

using SynQt::ClientAddress;

class TestClientAddress : public QObject
{
    Q_OBJECT

private slots:
    void noTrustedProxyIgnoresTheHeader();
    void aTrustedPeerYieldsTheForwardedClient();
    void anUntrustedPeerIsTheClient();
    void appendedHopsDoNotBecomeTheClient();
    void trustedHopsAreSkippedRightToLeft();
    void cidrRangesAreHonored();
    void aMalformedHeaderFallsBackToThePeer();
    void trustsPeerAnswersForTheListAlone();
    void anIPv4MappedPeerIsStillTheSamePeer();
};

void TestClientAddress::noTrustedProxyIgnoresTheHeader()
{
    // The default, and every project that has ever run. Nothing is in front, so the
    // header is a field the client wrote and is worth exactly nothing.
    const ClientAddress resolver{QStringList{}};
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("203.0.113.9")},
                              QByteArrayLiteral("198.51.100.7")),
             QStringLiteral("203.0.113.9"));
}

void TestClientAddress::aTrustedPeerYieldsTheForwardedClient()
{
    const ClientAddress resolver{QStringList{QStringLiteral("10.0.0.1")}};
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("10.0.0.1")},
                              QByteArrayLiteral("198.51.100.7")),
             QStringLiteral("198.51.100.7"));
}

void TestClientAddress::anUntrustedPeerIsTheClient()
{
    // The attack. A visitor reaching the edge directly and writing the header itself, to
    // land in a per-IP bucket of their choosing.
    const ClientAddress resolver{QStringList{QStringLiteral("10.0.0.1")}};
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("203.0.113.9")},
                              QByteArrayLiteral("198.51.100.7")),
             QStringLiteral("203.0.113.9"));
}

void TestClientAddress::appendedHopsDoNotBecomeTheClient()
{
    // The subtler attack. A real visitor behind the real balancer, sending a header of
    // their own that the balancer then appends its view of them to. Only the rightmost
    // untrusted entry was vouched for by anything trusted.
    const ClientAddress resolver{QStringList{QStringLiteral("10.0.0.1")}};
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("10.0.0.1")},
                              QByteArrayLiteral("1.2.3.4, 5.6.7.8, 198.51.100.7")),
             QStringLiteral("198.51.100.7"));
}

void TestClientAddress::trustedHopsAreSkippedRightToLeft()
{
    const ClientAddress resolver{QStringList{QStringLiteral("10.0.0.1"),
                                             QStringLiteral("10.0.0.2")}};
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("10.0.0.1")},
                              QByteArrayLiteral("198.51.100.7, 10.0.0.2")),
             QStringLiteral("198.51.100.7"));
}

void TestClientAddress::cidrRangesAreHonored()
{
    const ClientAddress resolver{QStringList{QStringLiteral("10.0.0.0/24")}};
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("10.0.0.17")},
                              QByteArrayLiteral("198.51.100.7")),
             QStringLiteral("198.51.100.7"));
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("10.0.1.17")},
                              QByteArrayLiteral("198.51.100.7")),
             QStringLiteral("10.0.1.17"));
}

void TestClientAddress::aMalformedHeaderFallsBackToThePeer()
{
    const ClientAddress resolver{QStringList{QStringLiteral("10.0.0.1")}};
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("10.0.0.1")},
                              QByteArrayLiteral("not-an-address")),
             QStringLiteral("10.0.0.1"));
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("10.0.0.1")}, QByteArray{}),
             QStringLiteral("10.0.0.1"));
}

void TestClientAddress::trustsPeerAnswersForTheListAlone()
{
    const ClientAddress resolver{QStringList{QStringLiteral("10.0.0.0/24")}};
    QVERIFY(resolver.trustsPeer(QHostAddress{QStringLiteral("10.0.0.5")}));
    QVERIFY(!resolver.trustsPeer(QHostAddress{QStringLiteral("10.0.1.5")}));

    const ClientAddress none{QStringList{}};
    QVERIFY(!none.trustsPeer(QHostAddress{QStringLiteral("10.0.0.5")}));
}

void TestClientAddress::anIPv4MappedPeerIsStillTheSamePeer()
{
    // A dual-stack listener reports an IPv4 peer as ::ffff:10.0.0.1. Compared as written
    // it matches no IPv4 subnet, so a configured trust list would quietly become an empty
    // one and every visitor behind the balancer would share one cap bucket again. The
    // form of the address is not a fact about the visitor and must not decide anything.
    const ClientAddress resolver{QStringList{QStringLiteral("10.0.0.0/24")}};
    QVERIFY(resolver.trustsPeer(QHostAddress{QStringLiteral("::ffff:10.0.0.1")}));
    QCOMPARE(resolver.resolve(QHostAddress{QStringLiteral("::ffff:10.0.0.1")},
                              QByteArrayLiteral("198.51.100.7")),
             QStringLiteral("198.51.100.7"));

    // And the same on the way out. One visitor must not be two cap keys.
    const ClientAddress none{QStringList{}};
    QCOMPARE(none.resolve(QHostAddress{QStringLiteral("::ffff:203.0.113.9")}, QByteArray{}),
             QStringLiteral("203.0.113.9"));
}

QTEST_MAIN(TestClientAddress)
#include "tst_clientaddress.moc"
