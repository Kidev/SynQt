// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// struct ucred (SO_PEERCRED) requires _GNU_SOURCE, which must be defined before any
// system header is pulled in, hence before the Qt includes below.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include "meshserver.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>

#if defined(Q_OS_LINUX)
#  include <sys/socket.h>
#  include <unistd.h>
#elif defined(Q_OS_MACOS)
#  include <unistd.h>
#endif

namespace SynQt {

namespace {

QString peerEntityName(const QSslSocket *socket)
{
    const QStringList names{
        socket->peerCertificate().subjectInfo(QSslCertificate::CommonName)};
    return names.value(0);
}

// Verify the local-socket peer runs as the same OS user. The OS identifies the user,
// not the entity: on this transport identity is colocation trust, not authentication.
bool peerIsSameUser(QLocalSocket *socket)
{
    const qintptr descriptor{socket->socketDescriptor()};
    if (descriptor < 0) {
        return false;
    }
#if defined(Q_OS_LINUX)
    struct ucred credentials;
    socklen_t length{sizeof(credentials)};
    if (getsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_PEERCRED, &credentials,
                   &length) != 0) {
        return false;
    }
    return credentials.uid == geteuid();
#elif defined(Q_OS_MACOS)
    uid_t peerUid{0};
    gid_t peerGid{0};
    if (getpeereid(static_cast<int>(descriptor), &peerUid, &peerGid) != 0) {
        return false;
    }
    return peerUid == geteuid();
#else
    // No OS peer-credential API on this platform; the socket-file permission
    // restriction is the only guard. Colocation trust already assumes same-user.
    return true;
#endif
}

} // namespace

MeshServer::MeshServer(QObject *parent)
    : QObject{parent}
{
}

MeshServer::~MeshServer() = default;

bool MeshServer::listenMutualTls(const QHostAddress &address, quint16 port,
                                 const QSslCertificate &caCertificate,
                                 const QSslCertificate &localCertificate,
                                 const QSslKey &localKey)
{
    m_sslServer = new QSslServer{this};

    QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
    configuration.setCaCertificates({caCertificate});
    configuration.setLocalCertificate(localCertificate);
    configuration.setPrivateKey(localKey);
    // Mutual TLS: the peer must present a certificate that verifies against the CA.
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_sslServer->setSslConfiguration(configuration);

    connect(m_sslServer, &QSslServer::pendingConnectionAvailable,
            this, &MeshServer::onTlsConnectionPending);
    connect(m_sslServer, &QSslServer::errorOccurred, this,
            [this](QSslSocket *socket, QAbstractSocket::SocketError error) {
                Q_UNUSED(error)
                emit peerRejected(socket ? socket->errorString()
                                         : QStringLiteral("TLS handshake failed"));
            });
    connect(m_sslServer, &QSslServer::sslErrors, this,
            [this](QSslSocket *socket, const QList<QSslError> &errors) {
                Q_UNUSED(socket)
                emit peerRejected(errors.isEmpty() ? QStringLiteral("peer verification failed")
                                                   : errors.first().errorString());
            });

    if (!m_sslServer->listen(address, port)) {
        m_errorString = m_sslServer->errorString();
        return false;
    }
    return true;
}

bool MeshServer::listenLocal(const QString &socketName, const QString &colocatedEntity)
{
    m_colocatedEntity = colocatedEntity;
    m_localServer = new QLocalServer{this};
    // Restrict the socket file to the run-as user.
    m_localServer->setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(socketName);
    if (!m_localServer->listen(socketName)) {
        m_errorString = m_localServer->errorString();
        return false;
    }
    connect(m_localServer, &QLocalServer::newConnection,
            this, &MeshServer::onLocalConnectionPending);
    return true;
}

quint16 MeshServer::serverPort() const
{
    return m_sslServer ? m_sslServer->serverPort() : static_cast<quint16>(0);
}

QString MeshServer::localSocketFullName() const
{
    return m_localServer ? m_localServer->fullServerName() : QString{};
}

QString MeshServer::errorString() const
{
    return m_errorString;
}

void MeshServer::onTlsConnectionPending()
{
    while (QTcpSocket *pending{qobject_cast<QTcpSocket *>(m_sslServer->nextPendingConnection())}) {
        QSslSocket *socket{qobject_cast<QSslSocket *>(pending)};
        if (!socket) {
            pending->deleteLater();
            continue;
        }
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        // The handshake completed and the certificate verified against the CA; the
        // subject is the calling entity.
        emit peerConnected(socket, MeshPeer{peerEntityName(socket), true});
    }
}

void MeshServer::onLocalConnectionPending()
{
    while (QLocalSocket *socket{m_localServer->nextPendingConnection()}) {
        if (!peerIsSameUser(socket)) {
            emit peerRejected(QStringLiteral("local peer failed the OS credential check"));
            socket->abort();
            socket->deleteLater();
            continue;
        }
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        // Colocation trust, not authentication: the OS confirmed the same user, but
        // the entity name is the configured one and could be any same-user process.
        emit peerConnected(socket, MeshPeer{m_colocatedEntity, false});
    }
}

} // namespace SynQt
