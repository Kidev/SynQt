// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// struct ucred (SO_PEERCRED) requires _GNU_SOURCE, defined before any system header, and so
// before the Qt includes.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include "localpeer.h"

#include <QLocalSocket>

#if defined(Q_OS_LINUX)
#  include <sys/socket.h>
#  include <unistd.h>
#elif defined(Q_OS_MACOS)
#  include <unistd.h>
#endif

namespace SynQt {

quint64 currentEffectiveUser()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    return static_cast<quint64>(geteuid());
#else
    return 0;
#endif
}

bool localPeerRunsAs(qintptr descriptor, quint64 user)
{
    if (descriptor < 0) {
        return false;
    }
#if defined(Q_OS_LINUX)
    struct ucred credentials;
    socklen_t length{sizeof(credentials)};
    if (getsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_PEERCRED, &credentials,
                   &length) != 0) {
        return false;
    }
    return static_cast<quint64>(credentials.uid) == user;
#elif defined(Q_OS_MACOS)
    uid_t peerUid{0};
    gid_t peerGid{0};
    if (getpeereid(static_cast<int>(descriptor), &peerUid, &peerGid) != 0) {
        return false;
    }
    return static_cast<quint64>(peerUid) == user;
#else
    // No OS peer-credential API here: the socket file permissions are the only guard, and
    // colocation trust already assumes the same user.
    Q_UNUSED(user);
    return true;
#endif
}

bool localPeerRunsAsThisUser(const QLocalSocket *socket)
{
    return socket != nullptr && localPeerRunsAs(socket->socketDescriptor(),
                                                currentEffectiveUser());
}

} // namespace SynQt
