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

class Caller;
class MeshServer;

/// The owner side of one connect point. It instantiates the Source from the entity's
/// QML, enables remoting on a host node, and listens for consumers over the mesh.
/// Deny by default. A connection is added to the host only if the verified calling
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
    /// The Source this link acquires, minted or continued, keyed on the verified entity
    /// name. A consumer that opens a second link reaches what its first link has been
    /// using. On a shared owner that object is a mirror of the one Source the entity
    /// answers everyone from. Otherwise it is that entity's own Source. Returns nullptr on
    /// a load failure.
    QObject *sourceForPeer(const MeshPeer &peer, QString *error);
    /// The one Source a shared owner answers from, loaded on first use.
    QObject *sharedSource(QString *error);
    /// Drop one link's claim on its entity's Source, destroying it with the last link.
    void releasePeerSource(const QString &entity);

    ConnectPointConfig m_config;
    MeshCredentials m_credentials;
    QQmlEngine *m_engine;
    MeshServer *m_server{nullptr};
    /// The Source one consuming entity's links share. Counted for the same reason the edge
    /// counts sessions. It outlives the link that built it, so something has to end it, and
    /// that is the last link closing.
    struct PeerSource
    {
        int connections{0};
        QObject *source{nullptr};
    };
    QHash<QString, PeerSource> m_peerSources;
    /// On a shared owner, the single Source every peer's mirror answers through. Null on an
    /// owner that is not shared, which has one Source per peer and nothing to share.
    QObject *m_sharedSource{nullptr};
    /// The Caller in the shared Source's QML context, adopted per call into whoever is
    /// asking. Owned by the shared Source.
    Caller *m_sharedCaller{nullptr};
    /// No host node here. It is per link, parented to that link's device (see
    /// onPeerConnected), so a disconnect takes it with it.
    QHash<QString, QObject *> m_contextObjects;
    QString m_errorString;
};

} // namespace SynQt

#endif // SYNQT_CONNECTPOINTHOST_H
