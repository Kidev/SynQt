// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "connectpointhost.h"

#include "caller.h"
#include "meshserver.h"
#include "sourcefactory.h"
#include "tracer.h"

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
    // Both outcomes: a gate observed only through refusals looks healthy while refusing
    // everybody.
    connect(this, &ConnectPointHost::connectionRefused, this, [this](const QString &entity) {
        trace(Category::Authorization, Severity::Warning, QStringLiteral("consumer refused"),
              {{QStringLiteral("connectPoint"), m_config.name},
               {QStringLiteral("callingEntity"), entity}});
    });
    connect(this, &ConnectPointHost::consumerAttached, this, [this](const QString &entity) {
        trace(Category::Lifecycle, Severity::Info, QStringLiteral("consumer attached"),
              {{QStringLiteral("connectPoint"), m_config.name},
               {QStringLiteral("callingEntity"), entity}});
    });
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
    // Checked before create(), whose own "Component is not ready" says less than the error
    // below.
    QObject *source{component.isReady() ? component.create(context) : nullptr};
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

QObject *ConnectPointHost::sharedSource(QString *error)
{
    if (m_sharedSource) {
        return m_sharedSource;
    }
    // A shared Source's Caller starts as nobody and becomes the current caller for each
    // forwarded call (SynQt::Caller::adopt). Minted here so its context is built once, with
    // the Source.
    Caller *caller{Caller::forEntity(m_config.contract, QString{}, false, nullptr, this)};
    QObject *source{createSource(caller, this, error)};
    if (!source) {
        delete caller;
        return nullptr;
    }
    caller->setParent(source);
    SourceFactory::bindCaller(source, caller);
    // It holds the state for every peer, so no `<scope>` gate applies; each peer's mirror
    // gates.
    SourceFactory::holdsSharedState(source);
    m_sharedSource = source;
    m_sharedCaller = caller;
    return source;
}

QObject *ConnectPointHost::sourceForPeer(const MeshPeer &peer, QString *error)
{
    // One object per consuming entity, however many links it opens, parented to the host
    // because it outlives any link: the owner's own Source when the owner is not shared, or
    // a mirror of the shared Source. Either way this entity's links acquire it, and its
    // Caller is this entity.
    PeerSource &entry{m_peerSources[peer.entity]};
    if (entry.source) {
        return entry.source;
    }
    Caller *caller{Caller::forEntity(m_config.contract, peer.entity, peer.authenticated,
                                     nullptr, this)};
    QObject *source{nullptr};
    if (m_config.shared) {
        QObject *shared{sharedSource(error)};
        if (shared) {
            source = SourceFactory::create(m_config.contract, this);
            if (!source && error) {
                *error = QStringLiteral("no Source registered for contract %1")
                             .arg(m_config.contract);
            }
            if (source) {
                caller->setSource(source);
                SourceFactory::mirror(source, shared, caller);
            }
        }
    } else {
        source = createSource(caller, this, error);
        if (source) {
            caller->setSource(source);
            // The Source knows its Caller too, so a slot can name whom it answers when it
            // calls the next entity in the chain.
            SourceFactory::bindCaller(source, caller);
        }
    }
    if (!source) {
        delete caller;
        return nullptr;
    }
    caller->setParent(source);
    entry.source = source;
    return source;
}

void ConnectPointHost::releasePeerSource(const QString &entity)
{
    const auto entry{m_peerSources.find(entity)};
    if (entry == m_peerSources.end()) {
        return;
    }
    if (--entry->connections > 0) {
        return;
    }
    delete entry->source;
    m_peerSources.erase(entry);
}

bool ConnectPointHost::start()
{
    // A per-caller point builds nothing here: it mints a Source per accepted peer, with a
    // Caller bound to that entity (see onPeerConnected()).
    //
    // A shared point is built now. Its Source is the entity: it holds what outlives any
    // caller, and its `Component.onCompleted` is where the entity subscribes to what it
    // consumes and starts its work. Built lazily, it would miss everything before the first
    // connection.
    m_server = new MeshServer{this};
    connect(m_server, &MeshServer::peerConnected, this, &ConnectPointHost::onPeerConnected);

    if (m_config.endpoint.mode == MeshTransportMode::MutualTls) {
        const QSslCertificate ca{loadCertificate(m_credentials.caCertPath)};
        const QSslCertificate cert{loadCertificate(m_credentials.certPath)};
        const QSslKey key{loadPrivateKey(m_credentials.keyPath)};
        // Reported here, where the three paths are: a mesh owner without its own identity
        // cannot be connected to, and every consumer would otherwise fail to verify with no
        // hint.
        if (ca.isNull() || cert.isNull() || key.isNull()) {
            m_errorString = QStringLiteral("connect point %1 has no usable mesh identity "
                                           "(ca %2, cert %3, key %4); run 'synqt mesh init' "
                                           "and 'synqt mesh cert --all'")
                                .arg(m_config.name, m_credentials.caCertPath,
                                     m_credentials.certPath, m_credentials.keyPath);
            return false;
        }
        // And the same second check as on the public surfaces: a readable key is not
        // necessarily one this build's TLS backend can present (see unusableKeyReason).
        const QString unusable{unusableKeyReason(key)};
        if (!unusable.isEmpty()) {
            m_errorString = QStringLiteral("connect point %1 cannot present the key at %2: "
                                           "%3")
                                .arg(m_config.name, m_credentials.keyPath, unusable);
            return false;
        }
        if (!m_server->listenMutualTls(QHostAddress{m_config.endpoint.host},
                                       m_config.endpoint.port, ca, cert, key)) {
            m_errorString = m_server->errorString();
            return false;
        }
    } else {
        // Local socket, trusted by colocation. The peer name is the single configured
        // consumer.
        if (!m_server->listenLocal(m_config.endpoint.socketName,
                                   m_config.consumers.value(0))) {
            m_errorString = m_server->errorString();
            return false;
        }
    }

    if (m_config.shared) {
        QString error;
        if (sharedSource(&error) == nullptr) {
            m_errorString = error;
            return false;
        }
    }
    return true;
}

void ConnectPointHost::onPeerConnected(QIODevice *device, const MeshPeer &peer)
{
    // Deny by default. An owner accepts a connection only from a listed consumer.
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

    // Claimed before the Source is fetched and released when the link goes, so this
    // entity's Source lives exactly while it has a link.
    const QString entity{peer.entity};
    ++m_peerSources[entity].connections;
    connect(device, &QObject::destroyed, this,
            [this, entity]() { releasePeerSource(entity); });

    // Everything this link owns hangs off one object, with children added in destruction
    // order: the node first, then the socket. QObject destroys children in insertion order,
    // and QtRO writes a RemoveObject to every connection as a host node is destroyed, so
    // the socket must outlive the node. WebEdge::hostConnection does the same.
    QObject *link{new QObject{this}};
    QRemoteObjectHost *node{new QRemoteObjectHost{link}};
    device->setParent(link);
    // MeshServer hands over the device, so this ends the link: the socket drops, the object
    // above goes, and the node and socket are destroyed in that order. Connected to the
    // concrete socket, since QIODevice has no peer-disconnect signal.
    const auto endLink{[link]() { link->deleteLater(); }};
    if (QAbstractSocket *socket{qobject_cast<QAbstractSocket *>(device)}) {
        connect(socket, &QAbstractSocket::disconnected, link, endLink);
    } else if (QLocalSocket *local{qobject_cast<QLocalSocket *>(device)}) {
        connect(local, &QLocalSocket::disconnected, link, endLink);
    }

    // A Caller with the certificate-verified entity name, for the owner's per-slot
    // authorization.
    node->setHostUrl(QUrl{QStringLiteral("synqt-cp-%1:///%2")
                              .arg(m_config.name,
                                   QUuid::createUuid().toString(QUuid::WithoutBraces))},
                     QRemoteObjectHost::AllowExternalRegistration);
    QString error;
    QObject *source{sourceForPeer(peer, &error)};
    if (!source) {
        emit connectionRefused(peer.entity);
        device->close();
        link->deleteLater();
        return;
    }
    if (!node->enableRemoting(source, m_config.name)) {
        m_errorString = QStringLiteral("enableRemoting failed for connect point %1")
                            .arg(m_config.name);
    }
    node->addHostSideConnection(device);
}

} // namespace SynQt
