// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_WITHDRAWSOURCE_H
#define SYNQT_WITHDRAWSOURCE_H

#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QObject;
class QRemoteObjectHostBase;
QT_END_NAMESPACE

namespace SynQt {

/// Stops remoting \a source on \a node while the node's connections stay up, and returns
/// whether it was remoted there.
///
/// QRemoteObjectHostBase::disableRemoting() is not enough on a node that keeps serving. It
/// frees the sources QtRO built for the Source's models and child objects, and leaves them
/// registered by name, so the next request a consumer sends to one of those names is a call
/// through freed memory. This takes the names back as well.
bool withdrawSource(QRemoteObjectHostBase *node, QObject *source);

} // namespace SynQt

#endif // SYNQT_WITHDRAWSOURCE_H
