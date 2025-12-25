// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CLIENTADDRESS_H
#define SYNQT_CLIENTADDRESS_H

#include <QHostAddress>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace SynQt {

/// Which address is the visitor, when the peer might be a load balancer.
///
/// Every per-IP limit on the edge is only as good as its notion of "IP". Facing the
/// internet directly that is the peer address and nothing else can be believed. Behind a
/// balancer the peer is the same address for every visitor at once, so those limits would
/// either collapse onto it or stop limiting anything, and the visitor's real address is in
/// a header that any client can also write.
///
/// So the rule is two-sided and neither half is optional:
///
/// 1. The forwarding header is read only when the DIRECT PEER is one the deployment named
///    in `public.trusted_proxies`. From anyone else it is a field the client filled in.
/// 2. Within it, only the rightmost entry that is not itself a named hop is taken.
///    Entries to the left of that are whatever the client sent, because a balancer
///    appends what it saw rather than replacing what was there.
///
/// The default (an empty list) trusts nothing and answers the peer address, which is the
/// behaviour of every SynQt edge that predates this class.
class ClientAddress
{
public:
    /// `trustedProxies` are addresses or CIDR ranges (`10.0.0.1`, `10.0.0.0/24`).
    /// Unparseable entries are dropped: a typo must not silently widen trust.
    explicit ClientAddress(const QStringList &trustedProxies);

    /// Is this peer one whose forwarding header may be believed at all?
    bool trustsPeer(const QHostAddress &peer) const;

    /// The visitor's address, as the string every per-IP limit keys on.
    QString resolve(const QHostAddress &peer, const QByteArray &forwardedFor) const;

private:
    bool isTrusted(const QHostAddress &address) const;

    QList<QPair<QHostAddress, int>> m_trusted;
};

/// An IPv4-mapped IPv6 address as plain IPv4, and anything else unchanged.
///
/// A dual-stack listener reports an IPv4 peer as `::ffff:10.0.0.1`, which matches no IPv4
/// subnet and prints as neither address a deployment wrote down. Left alone it would turn
/// a configured trust list into an empty one and a per-IP cap into a per-form-of-address
/// cap, both silently, so every address entering the comparison goes through here first.
QHostAddress normalizedAddress(const QHostAddress &address);

} // namespace SynQt

#endif // SYNQT_CLIENTADDRESS_H
