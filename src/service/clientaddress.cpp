// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "clientaddress.h"

#include <QList>

namespace SynQt {

QHostAddress normalizedAddress(const QHostAddress &address)
{
    bool mapped{false};
    const quint32 asIPv4{address.toIPv4Address(&mapped)};
    if (mapped) {
        return QHostAddress{asIPv4};
    }
    return address;
}

ClientAddress::ClientAddress(const QStringList &trustedProxies)
{
    for (const QString &entry : trustedProxies) {
        const QString trimmed{entry.trimmed()};
        if (trimmed.isEmpty()) {
            continue;
        }
        // parseSubnet takes both forms: a bare address gets a full-length prefix, so
        // "10.0.0.1" and "10.0.0.0/24" share one path. A negative prefix means unreadable
        // input, and the entry is dropped.
        const QPair<QHostAddress, int> subnet{QHostAddress::parseSubnet(trimmed)};
        if (subnet.second >= 0) {
            m_trusted.append(subnet);
        }
    }
}

bool ClientAddress::isTrusted(const QHostAddress &address) const
{
    const QHostAddress candidate{normalizedAddress(address)};
    for (const QPair<QHostAddress, int> &subnet : m_trusted) {
        if (candidate.isInSubnet(subnet.first, subnet.second)) {
            return true;
        }
    }
    return false;
}

bool ClientAddress::trustsPeer(const QHostAddress &peer) const
{
    return isTrusted(peer);
}

QString ClientAddress::resolve(const QHostAddress &peer, const QByteArray &forwardedFor) const
{
    const QString peerAddress{normalizedAddress(peer).toString()};
    if (m_trusted.isEmpty() || !isTrusted(peer)) {
        return peerAddress;
    }

    // Right to left. The rightmost entry is what the nearest hop observed; each trusted hop
    // is skipped to reach what it observed. The first entry neither trusted nor malformed
    // is the visitor as vouched for by a trusted hop. Entries further left were written by
    // the client.
    const QList<QByteArray> hops{forwardedFor.split(',')};
    for (auto it{hops.crbegin()}; it != hops.crend(); ++it) {
        const QHostAddress parsed{QString::fromLatin1(it->trimmed())};
        if (parsed.isNull()) {
            // An unparsable hop ends the walk at the peer: nothing past it can be
            // attributed.
            return peerAddress;
        }
        if (!isTrusted(parsed)) {
            return normalizedAddress(parsed).toString();
        }
    }
    return peerAddress;
}

} // namespace SynQt
