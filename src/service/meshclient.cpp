// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "meshclient.h"

#include "deletesoon.h"

#include <QLocalSocket>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>

#include <algorithm>

namespace SynQt {

MeshClient::MeshClient(QObject *parent)
    : QObject{parent}
{
}

MeshClient::~MeshClient() = default;

void MeshClient::stop()
{
    m_retrying = false;
    if (m_retryTimer) {
        m_retryTimer->stop();
    }
}

bool MeshClient::connectMutualTls(const QHostAddress &address, quint16 port,
                                  const QString &ownerEntity,
                                  const QSslCertificate &caCertificate,
                                  const QSslCertificate &localCertificate,
                                  const QSslKey &localKey)
{
    m_address = address;
    m_port = port;
    m_ownerEntity = ownerEntity;
    m_caCertificate = caCertificate;
    m_localCertificate = localCertificate;
    m_localKey = localKey;
    openMutualTls();
    return true;
}

void MeshClient::openMutualTls()
{
    retireUnusedSocket();
    m_sslSocket = new QSslSocket{this};

    QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
    configuration.setCaCertificates({m_caCertificate});
    configuration.setLocalCertificate(m_localCertificate);
    configuration.setPrivateKey(m_localKey);
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_sslSocket->setSslConfiguration(configuration);

    // Captured rather than read from the member, so a signal arriving from a socket this
    // client has already moved on from cannot be mistaken for the current one's.
    QSslSocket *socket{m_sslSocket};
    connect(socket, &QSslSocket::encrypted, this, [this, socket]() {
        m_backoffMs = ReconnectBaseMs;
        emit connected(socket);
    });
    connect(socket, &QSslSocket::disconnected, this, [this, socket]() {
        // An established link that dropped: the owner restarted, or the network did.
        emit errorOccurred(QStringLiteral("mesh link to %1 closed").arg(m_ownerEntity));
        Q_UNUSED(socket)
        scheduleRetry();
    });
    connect(socket, &QSslSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
                emit errorOccurred(socket->errorString());
                scheduleRetry();
            });
    connect(socket, &QSslSocket::sslErrors, this,
            [this](const QList<QSslError> &errors) {
                emit errorOccurred(errors.isEmpty()
                                       ? QStringLiteral("peer verification failed")
                                       : errors.first().errorString());
                // No retry here: the handshake continues to fail or succeed on its own,
                // and whichever it does reports through the two handlers above. Retrying
                // from here as well would open a second socket for one failure.
            });

    // Verify the owner's certificate identifies the expected entity (its subject),
    // while connecting over the network address.
    socket->connectToHostEncrypted(m_address.toString(), m_port, m_ownerEntity);
}

bool MeshClient::connectLocal(const QString &socketName)
{
    m_socketName = socketName;
    openLocal();
    return true;
}

void MeshClient::openLocal()
{
    retireUnusedSocket();
    m_localSocket = new QLocalSocket{this};
    QLocalSocket *socket{m_localSocket};
    connect(socket, &QLocalSocket::connected, this, [this, socket]() {
        m_backoffMs = ReconnectBaseMs;
        emit connected(socket);
    });
    connect(socket, &QLocalSocket::disconnected, this, [this]() {
        emit errorOccurred(QStringLiteral("local mesh link closed"));
        scheduleRetry();
    });
    connect(socket, &QLocalSocket::errorOccurred, this,
            [this, socket](QLocalSocket::LocalSocketError) {
                emit errorOccurred(socket->errorString());
                scheduleRetry();
            });
    socket->connectToServer(m_socketName);
}

void MeshClient::retireUnusedSocket()
{
    // Only a socket nobody took: once connected() was emitted and the receiver reparented
    // the device (the entity runtime parents it to the node it feeds), that node owns it
    // and frees it when the node is retired. Freeing it here as well would pull the
    // transport out from under a node still using it.
    if (m_sslSocket && m_sslSocket->parent() == this) {
        m_sslSocket->disconnect(this);
        m_sslSocket->abort();
        deleteSoon(m_sslSocket);
    }
    m_sslSocket = nullptr;
    if (m_localSocket && m_localSocket->parent() == this) {
        m_localSocket->disconnect(this);
        m_localSocket->abort();
        deleteSoon(m_localSocket);
    }
    m_localSocket = nullptr;
}

void MeshClient::scheduleRetry()
{
    if (!m_retrying) {
        return;
    }
    if (!m_retryTimer) {
        m_retryTimer = new QTimer{this};
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, [this]() {
            if (!m_socketName.isEmpty()) {
                openLocal();
            } else {
                openMutualTls();
            }
        });
    }
    if (m_retryTimer->isActive()) {
        return;  // one failure can be reported by more than one signal
    }
    m_retryTimer->start(m_backoffMs);
    m_backoffMs = std::min(m_backoffMs * 2, ReconnectMaxMs);
}

QIODevice *MeshClient::device() const
{
    if (m_sslSocket) {
        return m_sslSocket;
    }
    return m_localSocket;
}

} // namespace SynQt
