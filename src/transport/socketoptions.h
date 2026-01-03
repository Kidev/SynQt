// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SOCKETOPTIONS_H
#define SYNQT_SOCKETOPTIONS_H

#include <QtCore/qglobal.h>

QT_BEGIN_NAMESPACE
class QAbstractSocket;
QT_END_NAMESPACE

namespace SynQt {

/// Turn off Nagle's algorithm on a connected TCP socket.
///
/// Every SynQt socket carries small push frames with no bulk transfer to batch them into.
/// Nagle holds a small segment until the previous one is acknowledged, and the peer's
/// delayed ACK holds that acknowledgement back, which delays a frame that was ready to leave.
///
/// Qt sets this on a socket QWebSocket dials out on, but not on an accepted one. SynQt sets
/// it on both, and on mesh links in both directions.
///
/// A no-op on a socket that is not connected yet: call it from an accept handler, or from
/// QAbstractSocket::connected on a socket being dialled out.
void disableNagle(QAbstractSocket *socket);

} // namespace SynQt

#endif // SYNQT_SOCKETOPTIONS_H
