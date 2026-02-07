// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "entityruntime.h"

#include "connectpointhost.h"
#include "log.h"
#include "meshclient.h"
#include "proxypolicy.h"
#include "tracer.h"

#include "consumerbase.h"
#include "consumerfactory.h"
#include "deletesoon.h"

#include "cache.h"
#include "cachefactory.h"
#include "db.h"
#include "docs.h"
#include "documentfactory.h"
#include "http.h"
#include "icacheprovider.h"
#include "idocumentprovider.h"
#include "ipersistenceprovider.h"
#include "jobs.h"
#include "persistencefactory.h"
#include "providerconfig.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QSslCertificate>
#include <QSslKey>

#include <utility>

namespace SynQt {

namespace {

// Resolve a provider/settings map (from the topology) into a ProviderConfig. A value of the
// form "env:VAR" is read from the entity environment (secrets never live as literals in the
// resolved topology). Fields absent from the map keep their ProviderConfig defaults.
QString resolveEnv(const QVariant &value)
{
    const QString text{value.toString()};
    if (text.startsWith(QLatin1String("env:"))) {
        return qEnvironmentVariable(text.mid(4).toUtf8().constData());
    }
    return text;
}

ProviderConfig providerConfigFromMap(const QVariantMap &map)
{
    ProviderConfig config;
    config.name = map.value(QStringLiteral("name")).toString();
    config.file = map.value(QStringLiteral("file"), config.file).toString();
    config.journalMode = map.value(QStringLiteral("journal_mode"), config.journalMode).toString();
    config.busyTimeoutMs =
        map.value(QStringLiteral("busy_timeout_ms"), config.busyTimeoutMs).toInt();
    config.host = map.value(QStringLiteral("host"), config.host).toString();
    config.port = map.value(QStringLiteral("port"), config.port).toInt();
    config.database = map.value(QStringLiteral("database"), config.database).toString();
    config.user = map.value(QStringLiteral("user"), config.user).toString();
    config.sslMode = map.value(QStringLiteral("sslmode"), config.sslMode).toString();
    config.caCert = map.value(QStringLiteral("ca_cert"), config.caCert).toString();
    config.poolSize = map.value(QStringLiteral("pool_size"), config.poolSize).toInt();
    config.tls = map.value(QStringLiteral("tls"), config.tls).toBool();
    config.release = map.value(QStringLiteral("release"), config.release).toBool();
    if (map.contains(QStringLiteral("password"))) {
        config.password = resolveEnv(map.value(QStringLiteral("password")));
    }
    if (map.contains(QStringLiteral("uri"))) {
        config.uri = resolveEnv(map.value(QStringLiteral("uri")));
    }
    return config;
}

} // namespace

EntityRuntime::EntityRuntime(Topology topology, QQmlEngine *engine, QObject *parent)
    : QObject{parent}
    , m_topology{std::move(topology)}
    , m_engine{engine}
{
}

EntityRuntime::~EntityRuntime() = default;

bool EntityRuntime::buildTypeContext()
{
    const QString type{m_topology.type};
    if (type == QLatin1String("relational")) {
        m_persistence = makePersistenceProvider(providerConfigFromMap(m_topology.provider),
                                                &m_errorString);
        if (m_persistence == nullptr) {
            return false;
        }
        QString error;
        if (!m_persistence->connect(&error)) {
            m_errorString = error;
            m_persistence.reset();
            return false;
        }
        // A schema that did not apply is fatal. Every Source on this entity is written
        // against it, so starting would only move the failure to the first query.
        if (!m_topology.schema.isEmpty() && !m_persistence->migrate(m_topology.schema, &error)) {
            m_errorString = error;
            m_persistence.reset();
            return false;
        }
        m_typeContext.insert(QStringLiteral("Db"), new Db{m_persistence.get(), this});
    } else if (type == QLatin1String("cache")) {
        m_cache = makeCacheProvider(providerConfigFromMap(m_topology.provider), &m_errorString);
        if (m_cache == nullptr) {
            return false;
        }
        // Unlike a database, an unreachable cache is not fatal. A cache miss is a normal
        // outcome, the provider reports isHealthy(), and an external engine may come up
        // after the entity does. It is still said out loud, never swallowed.
        QString error;
        if (!m_cache->connect(&error)) {
            qWarning("SynQt: cache provider '%s' is not connected: %s",
                     qUtf8Printable(m_cache->name()), qUtf8Printable(error));
        }
        m_typeContext.insert(QStringLiteral("Cache"), new Cache{m_cache.get(), this});
    } else if (type == QLatin1String("document")) {
        m_document = makeDocumentProvider(providerConfigFromMap(m_topology.provider),
                                          &m_errorString);
        if (m_document == nullptr) {
            return false;
        }
        QString error;
        if (!m_document->connect(&error)) {
            qWarning("SynQt: document provider '%s' is not connected: %s",
                     qUtf8Printable(m_document->name()), qUtf8Printable(error));
        }
        m_typeContext.insert(QStringLiteral("Docs"), new Docs{m_document.get(), this});
    } else if (type == QLatin1String("jobs")) {
        m_typeContext.insert(QStringLiteral("Jobs"), new Jobs{1000, this});
    }

    // For every type, unlike the helpers above. Those exist because a type has an engine
    // behind it, and are absent where there is none. Every entity has something to say
    // about itself, so every entity gets this one.
    m_typeContext.insert(QStringLiteral("Log"), new Log{this});
    // The name this process records under, set once here rather than passed to every call
    // site. It is also why an entity cannot claim to be another one: the stamp is applied
    // on the way out of the pipeline, past anything QML can reach.
    Tracer::instance()->setEntity(m_topology.entity);

    // `Http` is granted by the topology, not by the type: any entity that declares
    // `network.outbound` gets it, restricted to exactly the prefixes in that list, and an
    // entity that declares none does not get it at all. An empty list still installs the
    // helper and allows nothing, so a call names the config key it is missing instead of
    // dying on an undefined `Http`. An entity is closed until a deployment opens it, and it
    // is opened onto named places rather than onto the internet.
    if (m_topology.outboundDeclared) {
        m_network = new QNetworkAccessManager{this};
        applyEnvironmentProxy(m_network);
        const bool release{m_topology.provider.value(QStringLiteral("release"), true).toBool()};
        // A declared header may be an `env:` reference, and this is where it stops being
        // one: read from this process's environment, held in the helper, and attached to
        // the request. It is never written to the resolved topology, never reaches the
        // entity's QML, and so cannot be logged by it.
        QList<HttpEndpointConfig> endpoints;
        endpoints.reserve(m_topology.outbound.size());
        for (const OutboundEndpoint &declared : std::as_const(m_topology.outbound)) {
            HttpEndpointConfig endpoint;
            endpoint.name = declared.name;
            endpoint.url = declared.url;
            for (auto it{declared.headers.constBegin()};
                 it != declared.headers.constEnd(); ++it) {
                endpoint.headers.insert(it.key(), resolveEnv(it.value()));
            }
            endpoints.append(endpoint);
        }
        m_typeContext.insert(QStringLiteral("Http"),
                             new Http{m_network, m_engine, release, endpoints, this});
    }
    return true;
}

QString EntityRuntime::accessorName(const QString &owner)
{
    if (owner.isEmpty()) {
        return owner;
    }
    return owner.left(1).toUpper() + owner.mid(1);
}

QString EntityRuntime::errorString() const
{
    return m_errorString;
}

void EntityRuntime::setContextObject(const QString &name, QObject *object)
{
    m_entityContext.insert(name, object);
}

QList<ConnectPointHost *> EntityRuntime::ownedHosts() const
{
    return m_ownedHosts;
}

void EntityRuntime::installAccessor(const ConnectPointConfig &connectPoint)
{
    if (connectPoint.framework) {
        return;
    }
    const QString name{accessorName(connectPoint.owner)};
    if (m_accessors.contains(name)) {
        return;
    }
    // The facade is what QML talks to. It forwards properties, models and signals,
    // turns a returning slot into a promise, and feeds the `<Contract>.on<Signal>` attached
    // handlers. It is built here, before the link, and kept for the life of the runtime, so
    // a reconnect hands the same object a fresh Replica and every binding against it holds.
    ConsumerBase *facade{makeConsumer(connectPoint.contract)};
    if (facade == nullptr) {
        // No consumer surface registered for this contract (a Replica-only build). There is
        // nothing to put in scope until a link acquires the dynamic Replica itself.
        return;
    }
    facade->setPoint(connectPoint.name);
    facade->setParent(this);
    m_consumerFacades.insert(connectPoint.owner + QLatin1Char('/') + connectPoint.name,
                             facade);
    m_accessors.insert(name, facade);
    if (m_engine) {
        m_engine->rootContext()->setContextProperty(name, facade);
    }
}

QObject *EntityRuntime::accessor(const QString &capitalizedOwner) const
{
    return m_accessors.value(capitalizedOwner);
}

QObject *EntityRuntime::consumedReplica(const QString &owner, const QString &connectPoint) const
{
    return m_consumedReplicas.value(owner + QLatin1Char('/') + connectPoint);
}

bool EntityRuntime::start()
{
    // Build the type's backend once, so every owned Source is created with its helper
    // (Db/Cache/Docs/Http/Jobs) already in context. An entity that cannot serve its type
    // never reaches enableRemoting(): a consumer being refused acquisition is a far better
    // failure than one acquiring a Source whose every call will fail.
    if (!buildTypeContext()) {
        return false;
    }

    // The helper goes on the ROOT context as well as on each Source's, because the entity's
    // own singleton is created by the engine in the root context and it is the entity: it is
    // where state that outlives any one Source belongs, and it cannot hold that state if it
    // cannot reach the engine behind it. Without this, `Db.exec(...)` in an entity singleton
    // is a ReferenceError that reads like a working line. Each Source's own context sets the
    // same objects again, which is what keeps the shadowing check below meaningful.
    // The same is true of anything the entity's main contributed (`Api` for an inbound
    // surface, the auth entity's engines). The singleton is where routes are declared and
    // where startup work happens, so what it needs has to be in scope there too.
    if (m_engine) {
        for (auto it{m_typeContext.constBegin()}; it != m_typeContext.constEnd(); ++it) {
            m_engine->rootContext()->setContextProperty(it.key(), it.value());
        }
        for (auto it{m_entityContext.constBegin()}; it != m_entityContext.constEnd(); ++it) {
            if (m_typeContext.contains(it.key())) {
                continue;  // reported once per owned Source below. Not twice more here
            }
            m_engine->rootContext()->setContextProperty(it.key(), it.value());
        }
    }

    // Every owner this entity consumes goes into QML scope before the first Source is
    // built, because a shared entity builds one at start-up and a binding in it against an
    // accessor that does not exist yet reads as nothing for good. Opening the links happens
    // further down. This is only the name coming into scope.
    for (const ConnectPointConfig &connectPoint : m_topology.consumed()) {
        installAccessor(connectPoint);
    }

    // Bring up an owner for every connect point this entity owns.
    for (const ConnectPointConfig &connectPoint : m_topology.owned()) {
        ConnectPointHost *host{
            new ConnectPointHost{connectPoint, m_topology.credentials, m_engine, this}};
        for (auto it{m_typeContext.constBegin()}; it != m_typeContext.constEnd(); ++it) {
            host->setContextObject(it.key(), it.value());
        }
        for (auto it{m_entityContext.constBegin()}; it != m_entityContext.constEnd(); ++it) {
            // The type's own helper wins. An entity contributing its own `Db` would leave
            // every Source on it calling something other than the provider the config
            // selected, and silently, because the name still resolves. Refusing the
            // override and saying so is the only outcome that cannot look like it worked.
            if (m_typeContext.contains(it.key())) {
                qWarning("SynQt: entity '%s' contributed '%s', which its %s type already "
                         "provides; keeping the type's helper",
                         qUtf8Printable(m_topology.entity), qUtf8Printable(it.key()),
                         qUtf8Printable(m_topology.type));
                continue;
            }
            host->setContextObject(it.key(), it.value());
        }
        connect(host, &ConnectPointHost::connectionRefused, this,
                [this, name = connectPoint.name](const QString &entity) {
                    emit connectionRefused(name, entity);
                });
        if (!host->start()) {
            m_errorString = host->errorString();
            return false;
        }
        m_ownedHosts.append(host);
    }

    // Open a consumer link for every connect point this entity consumes, and only
    // those (deny by default, no link to an owner this entity does not consume from).
    for (const ConnectPointConfig &connectPoint : m_topology.consumed()) {
        openConsumerLink(connectPoint);
    }
    return true;
}

void EntityRuntime::openConsumerLink(const ConnectPointConfig &connectPoint)
{
    // The owner goes into QML scope now, not when the handshake finishes. A binding in this
    // entity's QML is evaluated on its first frame, and a name that resolves to nothing
    // then reads as nothing for good.
    installAccessor(connectPoint);

    MeshClient *client{new MeshClient{this}};

    connect(client, &MeshClient::connected, this,
            [this, connectPoint](QIODevice *device) {
                const QString key{connectPoint.owner + QLatin1Char('/') + connectPoint.name};
                QRemoteObjectNode *node{new QRemoteObjectNode{this}};
                // The node owns the transport it was handed, so retiring the node below
                // takes the socket with it, in that order (a node tears down its own
                // connections before its children are destroyed). MeshClient hands
                // ownership to whoever takes the device, and this is where it is taken.
                device->setParent(node);
                node->addClientSideConnection(device);
                node->setHeartbeatInterval(1000);
                QRemoteObjectDynamicReplica *replica{node->acquireDynamic(connectPoint.name)};
                replica->setParent(node);
                // A reconnect is a new node, a new replica and a new transport. The one
                // this link used before is finished with. Retired after this turn, so the
                // facade below has already been pointed at the fresh Replica and nothing
                // still on the stack is reading the old one.
                if (QRemoteObjectNode *previous{m_consumedNodes.value(key)}) {
                    deleteSoon(previous);
                }
                m_consumedNodes.insert(key, node);
                m_consumedReplicas.insert(key, replica);

                // Announce the Replica once it can be connected to. A dynamic
                // Replica has no signals or slots until it is initialized, so C++ that
                // adopts one (the edge's IdentityProvider and SessionManager) has to wait
                // for this rather than for the transport.
                connect(replica, &QRemoteObjectDynamicReplica::initialized, this,
                        [this, connectPoint, replica]() {
                            emit consumedReplicaReady(connectPoint.owner, connectPoint.name,
                                                      replica);
                        });

                // Point the consumer facade at the fresh Replica. The facade is what QML
                // reaches this owner through (returning-slot promises,
                // `<Contract>.on<Signal>`), and installAccessor built it before the link,
                // so this is the same object across every reconnect.
                if (ConsumerBase *existing{m_consumerFacades.value(key)}) {
                    existing->setReplica(replica);
                    return;
                }
                // No facade for this contract, and nothing else this entity's QML could
                // reach the owner through: the raw dynamic Replica takes the name, once
                // there is one. A framework point takes none, because the C++ that adopts
                // it does so through consumedReplicaReady above.
                if (!connectPoint.framework && m_engine
                        && !m_accessors.contains(accessorName(connectPoint.owner))) {
                    m_accessors.insert(accessorName(connectPoint.owner), replica);
                    m_engine->rootContext()->setContextProperty(
                        accessorName(connectPoint.owner), replica);
                }
            });

    if (connectPoint.endpoint.mode == MeshTransportMode::MutualTls) {
        client->connectMutualTls(QHostAddress{connectPoint.endpoint.host},
                                 connectPoint.endpoint.port, connectPoint.owner,
                                 loadCertificate(m_topology.credentials.caCertPath),
                                 loadCertificate(m_topology.credentials.certPath),
                                 loadPrivateKey(m_topology.credentials.keyPath));
    } else {
        client->connectLocal(connectPoint.endpoint.socketName);
    }
}

} // namespace SynQt
