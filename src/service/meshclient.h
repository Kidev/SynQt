// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MESHCLIENT_H
#define SYNQT_MESHCLIENT_H

#include <QHostAddress>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QIODevice;
class QLocalSocket;
class QSslCertificate;
class QSslKey;
class QSslSocket;
QT_END_NAMESPACE

namespace SynQt {

/// The consumer side of a mesh link: it connects to an owner and, once the transport
/// is up, emits the QIODevice for the entity runtime to hand to a QtRO node with
/// addClientSideConnection(). Mutual TLS is the default; the local socket is an
/// explicit opt-in.
class MeshClient : public QObject
{
    Q_OBJECT

public:
    explicit MeshClient(QObject *parent = nullptr);
    ~MeshClient() override;

    /// Mutual TLS: present this entity's certificate, verify the owner against the
    /// project CA, and verify the owner's certificate identifies `ownerEntity` (its
    /// certificate subject), not merely the network address.
    bool connectMutualTls(const QHostAddress &address, quint16 port,
                          const QString &ownerEntity,
                          const QSslCertificate &caCertificate,
                          const QSslCertificate &localCertificate,
                          const QSslKey &localKey);

    /// Local socket (explicit opt-in). No certificate; trust is by colocation.
    bool connectLocal(const QString &socketName);

    QIODevice *device() const;

signals:
    void connected(QIODevice *device);
    void errorOccurred(const QString &reason);

private:
    QSslSocket *m_sslSocket{nullptr};
    QLocalSocket *m_localSocket{nullptr};
};

} // namespace SynQt

#endif // SYNQT_MESHCLIENT_H
