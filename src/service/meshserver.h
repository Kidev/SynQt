// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MESHSERVER_H
#define SYNQT_MESHSERVER_H

#include "meshpeer.h"

#include <QHostAddress>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QIODevice;
class QLocalServer;
class QSslCertificate;
class QSslKey;
class QSslServer;
QT_END_NAMESPACE

namespace SynQt {

/// The owner side of a mesh link: it listens, authenticates each peer, and emits the
/// accepted QIODevice for the entity runtime to hand to a QtRO host with
/// addHostSideConnection(). Mutual TLS is the default on every link (loopback for a
/// same-host link is just binding to the loopback address). The local socket is an
/// explicit opt-in and is never selected implicitly.
class MeshServer : public QObject
{
    Q_OBJECT

public:
    explicit MeshServer(QObject *parent = nullptr);
    ~MeshServer() override;

    /// Mutual TLS: verify every peer against the project CA and present this entity's
    /// certificate. A rejected peer (no certificate, or one from another CA) fails the
    /// TLS handshake and never reaches peerConnected().
    bool listenMutualTls(const QHostAddress &address, quint16 port,
                         const QSslCertificate &caCertificate,
                         const QSslCertificate &localCertificate,
                         const QSslKey &localKey);

    /// Local socket (explicit opt-in): restrict the socket file to the run-as user and
    /// check the peer's OS credentials. The peer entity is trusted by colocation
    /// (`colocatedEntity`), not authenticated.
    bool listenLocal(const QString &socketName, const QString &colocatedEntity);

    quint16 serverPort() const;
    QString localSocketFullName() const;
    QString errorString() const;

signals:
    void peerConnected(QIODevice *device, const SynQt::MeshPeer &peer);
    void peerRejected(const QString &reason);

private:
    void onTlsConnectionPending();
    void onLocalConnectionPending();

    QSslServer *m_sslServer{nullptr};
    QLocalServer *m_localServer{nullptr};
    QString m_colocatedEntity;
    QString m_errorString;
};

} // namespace SynQt

#endif // SYNQT_MESHSERVER_H
