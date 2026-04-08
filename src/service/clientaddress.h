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

/// Which address is the caller, when the peer might be a load balancer.
///
/// Facing the internet directly, the peer address is the only one to believe. Behind a
/// balancer the peer is the same address for every caller, and the real address is in a header
/// any client can also write. Two rules apply:
///
/// 1. The forwarding header is read only when the direct peer is in the deployment's
///    trusted-proxy list. From anyone else it is a field the client filled in.
/// 2. Within it, only the rightmost entry that is not itself a named hop is taken. Entries to
///    its left are whatever the client sent, because a balancer appends rather than replaces.
///
/// The default (an empty list) trusts nothing and answers the peer address.
///
/// The browser side reads `public.trusted_proxies` and the inbound API surface reads
/// `network.inbound.trusted_proxies`; neither inherits the other's list. It lives in the
/// service library so an API entity can use it without linking Qt HTTP Server.
class ClientAddress
{
public:
    /// `trustedProxies` are addresses or CIDR ranges (`10.0.0.1`, `10.0.0.0/24`).
    /// Unparseable entries are dropped. A typo must not silently widen trust.
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
/// subnet. Every address entering a trust-list or per-IP comparison goes through here first.
QHostAddress normalizedAddress(const QHostAddress &address);

} // namespace SynQt

#endif // SYNQT_CLIENTADDRESS_H
