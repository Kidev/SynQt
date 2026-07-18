// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "meshclient.h"

#include <QLocalSocket>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>

namespace SynQt {

MeshClient::MeshClient(QObject *parent)
    : QObject{parent}
{
}

MeshClient::~MeshClient() = default;

bool MeshClient::connectMutualTls(const QHostAddress &address, quint16 port,
                                  const QString &ownerEntity,
                                  const QSslCertificate &caCertificate,
                                  const QSslCertificate &localCertificate,
                                  const QSslKey &localKey)
{
    m_sslSocket = new QSslSocket{this};

    QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
    configuration.setCaCertificates({caCertificate});
    configuration.setLocalCertificate(localCertificate);
    configuration.setPrivateKey(localKey);
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_sslSocket->setSslConfiguration(configuration);

    connect(m_sslSocket, &QSslSocket::encrypted, this, [this]() {
        emit connected(m_sslSocket);
    });
    connect(m_sslSocket, &QSslSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                emit errorOccurred(m_sslSocket->errorString());
            });
    connect(m_sslSocket, &QSslSocket::sslErrors, this,
            [this](const QList<QSslError> &errors) {
                emit errorOccurred(errors.isEmpty()
                                       ? QStringLiteral("peer verification failed")
                                       : errors.first().errorString());
            });

    // Verify the owner's certificate identifies the expected entity (its subject),
    // while connecting over the network address.
    m_sslSocket->connectToHostEncrypted(address.toString(), port, ownerEntity);
    return true;
}

bool MeshClient::connectLocal(const QString &socketName)
{
    m_localSocket = new QLocalSocket{this};
    connect(m_localSocket, &QLocalSocket::connected, this, [this]() {
        emit connected(m_localSocket);
    });
    connect(m_localSocket, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) {
                emit errorOccurred(m_localSocket->errorString());
            });
    m_localSocket->connectToServer(socketName);
    return true;
}

QIODevice *MeshClient::device() const
{
    if (m_sslSocket) {
        return m_sslSocket;
    }
    return m_localSocket;
}

} // namespace SynQt
