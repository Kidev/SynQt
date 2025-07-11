// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SERVERACCESSOR_H
#define SYNQT_SERVERACCESSOR_H

#include "synclientconfig.h"

#include <QHash>
#include <QList>
#include <QQmlPropertyMap>

QT_BEGIN_NAMESPACE
class QRemoteObjectNode;
QT_END_NAMESPACE

namespace SynQt {

class ConsumerBase;

/// The client's handle on the web edge: each connect point the client consumes appears
/// on it by name, as a live Replica. Replicas are acquired in C++ (the QtRO QML Node
/// type cannot take an externally connected transport). Exposed to QML as the well-known
/// \qmlServer.
///
/// \sa \ref qmlserver "the Server accessor page"
class ServerAccessor : public QQmlPropertyMap
{
    Q_OBJECT

public:
    explicit ServerAccessor(QList<ClientConnectPoint> connectPoints,
                            QObject *parent = nullptr);

    /// Acquire the Replica of each consumed connect point on the given node and present
    /// it by name. Called on every (re)connect so bindings resume on a fresh link.
    void bindNode(QRemoteObjectNode *node);

private slots:
    /// Re-publish a Replica once its QtRO handshake completes, so QML bindings re-evaluate.
    void onReplicaInitialized();

private:
    QList<ClientConnectPoint> m_connectPoints;
    QHash<QObject *, QString> m_pending; ///< raw replica -> connect-point name (fallback path)
    QHash<QString, ConsumerBase *> m_facades; ///< connect-point name -> stable facade
};

} // namespace SynQt

#endif // SYNQT_SERVERACCESSOR_H
