// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MESHCLIENT_H
#define SYNQT_MESHCLIENT_H

#include <QHostAddress>
#include <QObject>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>

QT_BEGIN_NAMESPACE
class QIODevice;
class QLocalSocket;
class QSslSocket;
class QTimer;
QT_END_NAMESPACE

namespace SynQt {

/// The consumer side of a mesh link: it connects to an owner and, once the transport
/// is up, emits the QIODevice for the entity runtime to hand to a QtRO node with
/// addClientSideConnection(). Mutual TLS is the default; the local socket is an
/// explicit opt-in.
///
/// The link is kept up. A first attempt that fails, and an established link that later
/// drops, are the same thing here: both retry with a capped exponential backoff until the
/// owner answers, and each success emits connected() again with a fresh device. Without
/// that, restarting one service would strand every consumer of it until each of those was
/// restarted too, in dependency order, which is not an operation anyone should have to
/// perform to deploy a service.
///
/// Ownership of an emitted device passes to whoever takes it: reparent it (the entity
/// runtime parents it to the node it hands it to), and it is freed with that owner when
/// the link is replaced. A device nobody took stays this object's and is freed here.
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

    /// Stop retrying. The link stays as it is; nothing is torn down.
    void stop();

signals:
    void connected(QIODevice *device);
    void errorOccurred(const QString &reason);

private:
    /// How a dropped or refused link is retried: the first attempt comes quickly, so a
    /// service restarting under a deploy is barely noticed, and repeated failure backs off
    /// to a rate that neither floods a listener nor an operator's logs.
    static constexpr int ReconnectBaseMs{500};
    static constexpr int ReconnectMaxMs{10000};

    void openMutualTls();
    void openLocal();
    void scheduleRetry();
    /// Free the current socket if it is still ours (nobody took the connection).
    void retireUnusedSocket();

    QSslSocket *m_sslSocket{nullptr};
    QLocalSocket *m_localSocket{nullptr};
    QTimer *m_retryTimer{nullptr};
    int m_backoffMs{ReconnectBaseMs};
    bool m_retrying{true};

    /// What a retry needs to open the link again.
    QHostAddress m_address;
    quint16 m_port{0};
    QString m_ownerEntity;
    QSslCertificate m_caCertificate;
    QSslCertificate m_localCertificate;
    QSslKey m_localKey;
    QString m_socketName;
};

} // namespace SynQt

#endif // SYNQT_MESHCLIENT_H
