// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SERVERACCESSOR_H
#define SYNQT_SERVERACCESSOR_H

#include "synclientconfig.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QQmlPropertyMap>

QT_BEGIN_NAMESPACE
class QJSEngine;
class QRemoteObjectNode;
class QRemoteObjectReplica;
QT_END_NAMESPACE

namespace SynQt {

class ConsumerBase;

/// The client's handle on what it consumes: the connect point of every owner it reaches, by
/// that owner's name, as a live Replica behind its consumer facade. Replicas are acquired in
/// C++ (the QtRO QML Node type cannot take an externally connected transport).
///
/// The generated main puts each one in QML scope under its owner's name, and the edge's under
/// \qmlServer as well. A client reaches exactly one edge and nothing else.
///
/// Each facade is built at construction, not when a link first comes up, so a binding written
/// against `Server` resolves on the first frame.
///
/// \sa \ref qmlserver "the Server accessor page"
class ServerAccessor : public QQmlPropertyMap
{
    Q_OBJECT

public:
    explicit ServerAccessor(QList<ClientConnectPoint> connectPoints,
                            QObject *parent = nullptr);

    /// The engine each facade's returning slots answer QML through (ConsumerBase::setJsEngine).
    void setJsEngine(QJSEngine *engine);

    /// Acquire the Replica of each consumed connect point on the given node and present
    /// it by name. Called on every (re)connect so bindings resume on a fresh link.
    void bindNode(QRemoteObjectNode *node);

    /// The object QML reaches one connect point through, or nullptr when this client
    /// consumes no point of that name. Stable for the life of the client wherever the
    /// contract has a consumer facade, which is every contract the build generates.
    QObject *point(const QString &name) const;

    /// The Replica currently behind one connect point, or nullptr before the first link or
    /// when this client consumes no point of that name. A reconnect replaces it.
    QRemoteObjectReplica *replica(const QString &name) const;

private slots:
    /// Re-publish a Replica once its QtRO handshake completes, so QML bindings re-evaluate.
    void onReplicaInitialized();

private:
    QList<ClientConnectPoint> m_connectPoints;
    QHash<QObject *, QString> m_pending; ///< raw replica -> connect-point name (fallback path)
    QHash<QString, ConsumerBase *> m_facades; ///< owner name -> stable facade
    QHash<QString, QPointer<QRemoteObjectReplica>> m_replicas; ///< owner name -> live Replica
};

} // namespace SynQt

#endif // SYNQT_SERVERACCESSOR_H
