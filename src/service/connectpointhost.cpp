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
    // Both halves, connected once. A gate watched only through its refusals looks healthy
    // when it is refusing everybody, which is how `identity.required` refused every
    // visitor for months. The accepted link is the event that says the gate still opens.
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
    // Asked before creating: create() on a component that failed to compile prints its own
    // "Component is not ready" first, which says less than the error built below.
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
    // The Caller in a shared Source's context starts as nobody and is made to be whoever is
    // calling, one forwarded call at a time (SynQt::Caller::adopt). It is minted here rather
    // than in a mirror so the QML context that names it is built once, with the Source.
    Caller *caller{Caller::forEntity(m_config.contract, QString{}, false, nullptr, this)};
    QObject *source{createSource(caller, this, error)};
    if (!source) {
        delete caller;
        return nullptr;
    }
    caller->setParent(source);
    SourceFactory::bindCaller(source, caller);
    // Holds the state for every peer at once, so no `<scope>` gate applies to it. Each
    // peer's mirror gates what that peer sees.
    SourceFactory::holdsSharedState(source);
    m_sharedSource = source;
    m_sharedCaller = caller;
    return source;
}

QObject *ConnectPointHost::sourceForPeer(const MeshPeer &peer, QString *error)
{
    // One object per consuming entity, whatever number of links it opens, parented to the
    // host because it outlives any one of them. What that object is depends on the owner:
    // its own Source when the owner is not shared, and a mirror of the one shared Source
    // when it is. Either way it is what this entity's links acquire, and the Caller bound
    // to it is this entity.
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
            // The Source is told its Caller as well as the other way round, so a slot can
            // name whoever it is answering when it calls on to the next entity in the chain.
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
    // A per-caller point instantiates nothing here. It mints a Source, with a Caller bound
    // to the calling entity, per accepted peer, so the owner can authorize each entity in
    // its slots. See onPeerConnected(). There is nothing to build before a caller exists.
    //
    // A shared point is the opposite case, and it is why this is not left to the first
    // caller. The Source is the entity. It holds what outlives any one caller, and its
    // `Component.onCompleted` is where an entity subscribes to what it consumes or starts
    // its own work. Built lazily, an entity would sit inert until somebody connected and
    // would have missed everything that happened before that.
    m_server = new MeshServer{this};
    connect(m_server, &MeshServer::peerConnected, this, &ConnectPointHost::onPeerConnected);

    if (m_config.endpoint.mode == MeshTransportMode::MutualTls) {
        const QSslCertificate ca{loadCertificate(m_credentials.caCertPath)};
        const QSslCertificate cert{loadCertificate(m_credentials.certPath)};
        const QSslKey key{loadPrivateKey(m_credentials.keyPath)};
        // Said here, where the three paths are, rather than left to present itself as every
        // consumer failing to verify. A mesh owner with no identity of its own is an owner
        // nothing can connect to, and the reason is which of these files it did not get.
        if (ca.isNull() || cert.isNull() || key.isNull()) {
            m_errorString = QStringLiteral("connect point %1 has no usable mesh identity "
                                           "(ca %2, cert %3, key %4); run 'synqt mesh init' "
                                           "and 'synqt mesh cert --all'")
                                .arg(m_config.name, m_credentials.caCertPath,
                                     m_credentials.certPath, m_credentials.keyPath);
            return false;
        }
        if (!m_server->listenMutualTls(QHostAddress{m_config.endpoint.host},
                                       m_config.endpoint.port, ca, cert, key)) {
            m_errorString = m_server->errorString();
            return false;
        }
    } else {
        // Local socket. Colocation-trusted. The peer name is the single configured
        // consumer (a local link is used for a co-located, equally trusted pair).
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

    // Claimed before the Source is reached for, and released when the link goes away, so
    // this entity's Source lives exactly as long as it has a link open.
    const QString entity{peer.entity};
    ++m_peerSources[entity].connections;
    connect(device, &QObject::destroyed, this,
            [this, entity]() { releasePeerSource(entity); });

    // Everything this link owns hangs off one object, and the two things under it are added
    // in the order they have to be destroyed in. The node first, the socket second. QObject
    // destroys its children in the order they were added, and QtRO writes a RemoveObject to
    // every connection as a host node goes, so the socket has to outlive the node that is
    // still talking to it. Hanging the node off the socket instead put that write after
    // ~QSslSocket had already run. WebEdge::hostConnection arranges a browser link the same
    // way and for the same reason.
    QObject *link{new QObject{this}};
    QRemoteObjectHost *node{new QRemoteObjectHost{link}};
    device->setParent(link);
    // MeshServer hands the device over rather than reclaiming it, so this is what ends the
    // link: the socket drops, the object above goes, and the node and the socket are
    // destroyed in that order. Connected to the concrete socket because QIODevice has no
    // notion of a peer hanging up.
    const auto endLink{[link]() { link->deleteLater(); }};
    if (QAbstractSocket *socket{qobject_cast<QAbstractSocket *>(device)}) {
        connect(socket, &QAbstractSocket::disconnected, link, endLink);
    } else if (QLocalSocket *local{qobject_cast<QLocalSocket *>(device)}) {
        connect(local, &QLocalSocket::disconnected, link, endLink);
    }

    // A Caller carrying the certificate-verified entity name, for the owner's per-slot
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
