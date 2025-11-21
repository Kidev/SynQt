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
/// entity is on this connect point's consumer allowlist. Any other peer is refused.
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
    /// The Source this link acquires, minted or continued. `PerCaller` (the default) keys
    /// it on the verified entity name, so a consumer that opens a second link reaches the
    /// Source its first link has been using. Returns nullptr on a load failure.
    QObject *sourceForPeer(const MeshPeer &peer, QIODevice *device, QString *error);
    /// Drop one link's claim on its entity's Source, destroying it with the last link.
    void releasePeerSource(const QString &entity);

    ConnectPointConfig m_config;
    MeshCredentials m_credentials;
    QQmlEngine *m_engine;
    MeshServer *m_server{nullptr};
    /// The Source one consuming entity's links share, under `PerCaller`. Counted for the
    /// same reason the edge counts sessions: it outlives the link that built it, so
    /// something has to end it, and that is the last link closing.
    struct PeerSource
    {
        int connections{0};
        QObject *source{nullptr};
    };
    QHash<QString, PeerSource> m_peerSources;
    /// No Source and no host node here: both are per peer, parented to that peer's device
    /// (see onPeerConnected), so a disconnect takes its Source and its Caller with it.
    QHash<QString, QObject *> m_contextObjects;
    QString m_errorString;
};

} // namespace SynQt

#endif // SYNQT_CONNECTPOINTHOST_H
