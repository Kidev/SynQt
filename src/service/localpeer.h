// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_LOCALPEER_H
#define SYNQT_LOCALPEER_H

#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QLocalSocket;
QT_END_NAMESPACE

namespace SynQt {

/// The user this process runs as, as the local peer checks compare it.
quint64 currentEffectiveUser();

/// Whether the process at the other end of a connected local socket runs as `user`,
/// asked of the operating system through the socket itself (SO_PEERCRED on Linux,
/// getpeereid on macOS). False when the platform cannot say or the socket is not one it
/// can say anything about. A platform with no such API answers true, which is the
/// pre-existing position of `transport: local` there (the socket file's permissions are
/// the only guard) and is stated in docs/security.md.
bool localPeerRunsAs(qintptr descriptor, quint64 user);

/// The check both ends of a local link run. The owner on each accepted socket, so a
/// process of another user cannot present itself as a consumer, and the consumer on the
/// socket it connected, so a process of another user squatting the socket's path cannot
/// present itself as the owner. The OS identifies the user and not the entity, so what
/// passes here is colocation trust, never authentication (docs/security.md).
bool localPeerRunsAsThisUser(const QLocalSocket *socket);

} // namespace SynQt

#endif // SYNQT_LOCALPEER_H
