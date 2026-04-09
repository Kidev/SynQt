// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_OBJECTTREE_H
#define SYNQT_OBJECTTREE_H

#include <QObject>

namespace SynQt {

/// Whether `candidate` already sits somewhere under `ancestor`.
///
/// Asked before adopting a socket. An accepted browser connection is two objects, the
/// QWebSocket and the raw socket under it, and which owns the other depends on how the
/// connection was made. Reparenting a raw socket Qt already placed under the QWebSocket takes
/// it from its owner; leaving an unowned one alone leaks the connection. The adoption is
/// conditional, and this is the condition.
///
/// Used by `SocketChannel` when it gathers a connection for an IO thread, and by the web edge
/// when the link has no channel.
inline bool isUnder(const QObject *candidate, const QObject *ancestor)
{
    for (const QObject *walk{candidate}; walk != nullptr; walk = walk->parent()) {
        if (walk == ancestor) {
            return true;
        }
    }
    return false;
}

} // namespace SynQt

#endif // SYNQT_OBJECTTREE_H
