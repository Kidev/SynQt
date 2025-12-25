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
        // parseSubnet takes both forms: a bare address comes back with a full-length
        // prefix, so one code path covers "10.0.0.1" and "10.0.0.0/24" alike. A prefix
        // below zero is what it reports for input it could not read, and that entry is
        // dropped rather than widened into something that matches.
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

    // Right to left: the rightmost entry is what the nearest hop observed, and each
    // trusted hop recognized along the way is skipped to reach what IT observed. The
    // first entry that is neither trusted nor malformed is the closest thing to the
    // visitor that anything we believe actually vouched for. Anything further left was
    // written by whoever the client is, so it is never the answer.
    const QList<QByteArray> hops{forwardedFor.split(',')};
    for (auto it{hops.crbegin()}; it != hops.crend(); ++it) {
        const QHostAddress parsed{QString::fromLatin1(it->trimmed())};
        if (parsed.isNull()) {
            // A hop nobody can parse ends the walk at the peer rather than at a guess:
            // the chain past it cannot be attributed to anyone.
            return peerAddress;
        }
        if (!isTrusted(parsed)) {
            return normalizedAddress(parsed).toString();
        }
    }
    return peerAddress;
}

} // namespace SynQt
