// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CONNECTPOINTHOST_H
#define SYNQT_CONNECTPOINTHOST_H

#include "meshpeer.h"
#include "topology.h"

#include <QHash>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QIODevice;
class QQmlEngine;
class QRemoteObjectHost;
QT_END_NAMESPACE

namespace SynQt {

class MeshServer;

/// The owner side of one connect point: it instantiates the Source from the entity's
/// QML, enables remoting on a host node, and listens for consumers over the mesh.
/// Deny by default: a connection is added to the host only if the verified calling
/// entity is on this connect point's consumer allowlist; any other peer is refused.
class ConnectPointHost : public QObject
{
    Q_OBJECT

public:
    ConnectPointHost(ConnectPointConfig config, MeshCredentials credentials,
                     QQmlEngine *engine, QObject *parent = nullptr);
    ~ConnectPointHost() override;

    bool start();
    QString errorString() const;
    QString name() const;
    quint16 serverPort() const;  ///< the mutual-TLS listen port (0 for a local socket)

    /// Expose an accessor (e.g. a consumed connect point or the Db helper) to every Source
    /// instance's QML context.
    void setContextObject(const QString &name, QObject *object);
    QObject *contextObject(const QString &name) const;

signals:
    /// A peer completed the transport handshake but was refused because it is not a
    /// listed consumer of this connect point (deny by default).
    void connectionRefused(const QString &entity);
    void consumerAttached(const QString &entity);

private:
    void onPeerConnected(QIODevice *device, const SynQt::MeshPeer &peer);
    QObject *createSource(QObject *caller, QObject *parent, QString *error);

    ConnectPointConfig m_config;
    MeshCredentials m_credentials;
    QQmlEngine *m_engine;
    QRemoteObjectHost *m_host{nullptr};  ///< the shared-instance host (null for per_peer)
    MeshServer *m_server{nullptr};
    QObject *m_source{nullptr};          ///< the shared-instance Source (null for per_peer)
    QHash<QString, QObject *> m_contextObjects;
    QString m_errorString;
};

} // namespace SynQt

#endif // SYNQT_CONNECTPOINTHOST_H
