// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "connectpointhost.h"

#include "caller.h"
#include "meshserver.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QIODevice>
#include <QLocalSocket>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRemoteObjectHost>
#include <QSslCertificate>
#include <QSslKey>
#include <QUrl>
#include <QUuid>

#include <utility>

namespace SynQt {

ConnectPointHost::ConnectPointHost(ConnectPointConfig config, MeshCredentials credentials,
                                   QQmlEngine *engine, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_credentials{std::move(credentials)}
    , m_engine{engine}
{
}

ConnectPointHost::~ConnectPointHost() = default;

QString ConnectPointHost::name() const
{
    return m_config.name;
}

QString ConnectPointHost::errorString() const
{
    return m_errorString;
}

quint16 ConnectPointHost::serverPort() const
{
    return m_server ? m_server->serverPort() : static_cast<quint16>(0);
}

void ConnectPointHost::setContextObject(const QString &name, QObject *object)
{
    m_contextObjects.insert(name, object);
}

QObject *ConnectPointHost::contextObject(const QString &name) const
{
    return m_contextObjects.value(name);
}

QObject *ConnectPointHost::createSource(QObject *caller, QObject *parent, QString *error)
{
    QQmlContext *context{new QQmlContext{m_engine->rootContext(), parent}};
    if (caller) {
        context->setContextProperty(QStringLiteral("Caller"), caller);
    }
    for (auto it{m_contextObjects.constBegin()}; it != m_contextObjects.constEnd(); ++it) {
        context->setContextProperty(it.key(), it.value());
    }
    QQmlComponent component{m_engine, QUrl::fromLocalFile(m_config.serverFile)};
    QObject *source{component.create(context)};
    if (!source) {
        if (error) {
            *error = QStringLiteral("failed to load %1: %2")
                         .arg(m_config.serverFile, component.errorString());
        }
        return nullptr;
    }
    source->setParent(parent);
    context->setParent(source);
    return source;
}

bool ConnectPointHost::start()
{
    // A shared connect point has one authoritative Source, hosted once; a per_peer connect
    // point mints a Source (with a Caller bound to the calling entity) per accepted peer,
    // so the owner can authorize each entity in its slots.
    if (m_config.instance == ConnectPointInstance::Shared) {
        QString error;
        m_source = createSource(nullptr, this, &error);
        if (!m_source) {
            m_errorString = error;
            return false;
        }
        m_host = new QRemoteObjectHost{this};
        m_host->setHostUrl(QUrl{QStringLiteral("synqt-cp-%1:///host").arg(m_config.name)},
                           QRemoteObjectHost::AllowExternalRegistration);
        if (!m_host->enableRemoting(m_source, m_config.name)) {
            m_errorString = QStringLiteral("enableRemoting failed for connect point %1")
                                .arg(m_config.name);
            return false;
        }
    }

    m_server = new MeshServer{this};
    connect(m_server, &MeshServer::peerConnected, this, &ConnectPointHost::onPeerConnected);

    if (m_config.endpoint.mode == MeshTransportMode::MutualTls) {
        const QSslCertificate ca{loadCertificate(m_credentials.caCertPath)};
        const QSslCertificate cert{loadCertificate(m_credentials.certPath)};
        const QSslKey key{loadPrivateKey(m_credentials.keyPath)};
        if (!m_server->listenMutualTls(QHostAddress{m_config.endpoint.host},
                                       m_config.endpoint.port, ca, cert, key)) {
            m_errorString = m_server->errorString();
            return false;
        }
    } else {
        // Local socket: colocation-trusted. The peer name is the single configured
        // consumer (a local link is used for a co-located, equally trusted pair).
        if (!m_server->listenLocal(m_config.endpoint.socketName,
                                   m_config.consumers.value(0))) {
            m_errorString = m_server->errorString();
            return false;
        }
    }
    return true;
}

void ConnectPointHost::onPeerConnected(QIODevice *device, const MeshPeer &peer)
{
    // Deny by default: an owner accepts a connection only from a listed consumer.
    if (!m_config.consumers.contains(peer.entity)) {
        emit connectionRefused(peer.entity);
        if (QAbstractSocket *socket{qobject_cast<QAbstractSocket *>(device)}) {
            socket->abort();
        } else if (QLocalSocket *local{qobject_cast<QLocalSocket *>(device)}) {
            local->abort();
        } else {
            device->close();
        }
        device->deleteLater();
        return;
    }
    emit consumerAttached(peer.entity);

    if (m_config.instance == ConnectPointInstance::Shared) {
        m_host->addHostSideConnection(device);
        return;
    }

    // per_peer: a fresh Source and its own node for this entity, with a Caller carrying
    // the certificate-verified entity name for the owner's per-slot authorization.
    QRemoteObjectHost *node{new QRemoteObjectHost{device}};
    node->setHostUrl(QUrl{QStringLiteral("synqt-cp-%1:///%2")
                              .arg(m_config.name,
                                   QUuid::createUuid().toString(QUuid::WithoutBraces))},
                     QRemoteObjectHost::AllowExternalRegistration);
    Caller *caller{Caller::forEntity(m_config.contract, peer.entity, peer.authenticated,
                                     nullptr, device)};
    QString error;
    QObject *source{createSource(caller, device, &error)};
    if (!source) {
        emit connectionRefused(peer.entity);
        device->close();
        device->deleteLater();
        return;
    }
    caller->setParent(source);
    caller->setSource(source);
    if (!node->enableRemoting(source, m_config.name)) {
        m_errorString = QStringLiteral("enableRemoting failed for connect point %1")
                            .arg(m_config.name);
    }
    node->addHostSideConnection(device);
}

} // namespace SynQt
