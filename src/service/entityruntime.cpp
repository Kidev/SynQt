// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "entityruntime.h"

#include "connectpointhost.h"
#include "ingestclient.h"
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

#include <algorithm>
#include <utility>

namespace SynQt {

namespace {

// Resolve a provider/settings map from the topology into a ProviderConfig. An "env:VAR"
// value is read from the entity environment, so secrets are never literals in the resolved
// topology. Absent fields keep their ProviderConfig defaults.
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
    config.synchronous = map.value(QStringLiteral("synchronous"), config.synchronous).toString();
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

EntityRuntime::~EntityRuntime()
{
    // The sink this runtime installed holds a raw pointer to a child of this object, and
    // the tracer outlives it: `Tracer::instance()` is a function-local static destroyed
    // after main's locals, and its destructor may deliver one last batch.
    //
    // Only this runtime's sink is cleared, never one something else installed, which is
    // also why buildIngest only ever enables the tracer.
    if (m_installedSink) {
        Tracer::instance()->setSink(Tracer::Sink{});
    }
}

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
        // A schema that did not apply is fatal: every Source here is written against it.
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
        // An unreachable cache is not fatal: a miss is normal, the provider reports
        // isHealthy(), and an external engine may start after the entity. It is still
        // logged.
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

    // Installed for every type, unlike the engine helpers above.
    m_typeContext.insert(QStringLiteral("Log"), new Log{this});
    // The name this process records under, set once. The stamp is applied as events leave
    // the pipeline, beyond QML's reach, so an entity cannot record as another.
    Tracer::instance()->setEntity(m_topology.entity);
    applyTraceLevels();
    buildIngest();

    // `Http` is granted by the topology, not the type: an entity that declares
    // `network.outbound` gets it, limited to those prefixes, and one that declares none
    // does not. An empty list still installs the helper and allows nothing, so a call names
    // the missing config key.
    if (m_topology.outboundDeclared) {
        m_network = new QNetworkAccessManager{this};
        applyEnvironmentProxy(m_network);
        const bool release{m_topology.provider.value(QStringLiteral("release"), true).toBool()};
        // A declared header may be an `env:` reference, resolved here from this process's
        // environment and held in the helper. It never reaches the resolved topology or the
        // entity's QML.
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

/// How much this entity records, from `monitoring.levels`.
///
/// Applied with or without a monitor, before the sink is installed: the levels also govern
/// a local exporter or a test harness. Unnamed categories keep their default.
///
/// An unknown level word is reported and its category keeps the default.
void EntityRuntime::applyTraceLevels()
{
    for (auto it{m_topology.traceLevels.constBegin()};
         it != m_topology.traceLevels.constEnd(); ++it) {
        Category category{Category::Application};
        if (!categoryFromName(it.key(), &category)) {
            qWarning().noquote() << "monitoring.levels: unknown category" << it.key();
            continue;
        }
        if (it.value() == QLatin1String("off")) {
            // Off refuses every severity, so it has its own setter.
            Tracer::instance()->setCategoryOff(category);
            continue;
        }
        Severity minimum{Severity::Info};
        if (!severityFromName(it.value(), &minimum)) {
            qWarning().noquote() << "monitoring.levels:" << it.key()
                                 << "has unknown level" << it.value();
            continue;
        }
        Tracer::instance()->setLevel(category, minimum);
    }
}

/// Point the tracer at the monitor, if this entity has one.
///
/// The client is built whether or not the link is up: an entity that starts before its
/// monitor spools until it arrives. The Replica is attached when the link comes up and
/// detached when it goes; the entity's own code sees neither.
void EntityRuntime::buildIngest()
{
    const bool reports{std::any_of(m_topology.connectPoints.cbegin(),
                                   m_topology.connectPoints.cend(),
                                   [this](const ConnectPointConfig &point) {
        return (point.name == QLatin1String("ingest"))
                && (point.owner != m_topology.entity);
    })};
    if (!reports) {
        // No monitor in this topology. The tracer is left as found: it starts off (see
        // Tracer::instance), and switching it off here would also silence a sink something
        // else installed (a local exporter, a test harness).
        return;
    }

    const QString spool{m_topology.spoolDir.isEmpty()
                            ? QString{}
                            : m_topology.spoolDir + QLatin1String("/monitoring.spool")};
    m_ingest = new IngestClient{spool, m_topology.spoolCapBytes, this};
    Tracer::instance()->setEnabled(true);
    // The sink runs on the tracer's writer thread, and IngestClient never waits on the
    // Replica's socket or on its file.
    IngestClient *ingest{m_ingest};
    Tracer::instance()->setSink([ingest](const QList<TraceEvent> &batch) {
        ingest->publish(batch);
    });
    m_installedSink = true;
    connect(this, &EntityRuntime::consumedReplicaReady, this,
            [this](const QString &, const QString &connectPoint, QObject *replica) {
        if (connectPoint == QLatin1String("ingest")) {
            m_ingest->setReplica(replica);
        }
    });
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
    // QML talks to the facade: it forwards properties, models and signals, turns a
    // returning slot into a promise, and feeds the `<Contract>.on<Signal>` handlers. Built
    // before the link and kept for the life of the runtime, so a reconnect gives the same
    // object a new Replica.
    ConsumerBase *facade{makeConsumer(connectPoint.contract)};
    if (facade == nullptr) {
        // No consumer surface for this contract (a Replica-only build); nothing goes in
        // scope until a link acquires the dynamic Replica.
        return;
    }
    facade->setPoint(connectPoint.name);
    facade->setJsEngine(m_engine);
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

QRemoteObjectDynamicReplica *EntityRuntime::consumedReplica(const QString &owner,
                                                            const QString &connectPoint) const
{
    return m_consumedReplicas.value(owner + QLatin1Char('/') + connectPoint);
}

bool EntityRuntime::start()
{
    // Build the type's backend once, so every owned Source is created with its helper (Db,
    // Cache, Docs, Http, Jobs) in context. An entity that cannot serve its type never
    // reaches enableRemoting(), so consumers are refused instead of acquiring a failing
    // Source.
    if (!buildTypeContext()) {
        return false;
    }

    // The helper goes on the root context as well as on each Source's. The entity singleton
    // is created in the root context and holds the state that outlives a Source, so it must
    // reach the engine; otherwise `Db.exec(...)` there is a ReferenceError. Each Source's
    // context sets the same objects again, which keeps the shadowing check below
    // meaningful.
    //
    // The same applies to what the entity main contributed (`Api`, the auth entity's
    // engines): the singleton declares routes and runs startup work.
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

    // Every consumed owner is put in QML scope before the first Source is built: a shared
    // entity builds one at start-up, and a binding against a missing accessor would stay
    // empty. The links open further down.
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
            // The type's own helper wins. An entity defining its own `Db` would make every
            // Source call something other than the configured provider, silently, so the
            // override is refused and reported.
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

    // Open a consumer link for every connect point this entity consumes, and only those
    // (deny by default).
    for (const ConnectPointConfig &connectPoint : m_topology.consumed()) {
        openConsumerLink(connectPoint);
    }
    return true;
}

void EntityRuntime::openConsumerLink(const ConnectPointConfig &connectPoint)
{
    // The owner name goes into QML scope now, not after the handshake: bindings evaluate on
    // the first frame.
    installAccessor(connectPoint);

    MeshClient *client{new MeshClient{this}};

    connect(client, &MeshClient::connected, this,
            [this, connectPoint](QIODevice *device) {
                const QString key{connectPoint.owner + QLatin1Char('/') + connectPoint.name};
                QRemoteObjectNode *node{new QRemoteObjectNode{this}};
                // The node owns the transport it was given, so retiring the node takes the
                // socket with it (a node closes its connections before its children are
                // destroyed). MeshClient hands ownership to whoever takes the device.
                device->setParent(node);
                node->addClientSideConnection(device);
                node->setHeartbeatInterval(1000);
                QRemoteObjectDynamicReplica *replica{node->acquireDynamic(connectPoint.name)};
                replica->setParent(node);
                // A reconnect is a new node, replica and transport. The old ones are
                // retired after this turn, once the facade points at the new Replica.
                if (QRemoteObjectNode *previous{m_consumedNodes.value(key)}) {
                    deleteSoon(previous);
                }
                m_consumedNodes.insert(key, node);
                m_consumedReplicas.insert(key, replica);

                // Announce the Replica once it can be connected to: a dynamic Replica has
                // no signals or slots until initialized, so C++ that adopts one (the edge's
                // IdentityProvider and SessionManager) waits for this.
                connect(replica, &QRemoteObjectDynamicReplica::initialized, this,
                        [this, connectPoint, replica]() {
                            emit consumedReplicaReady(connectPoint.owner, connectPoint.name,
                                                      replica);
                        });

                // Point the consumer facade at the new Replica. installAccessor built it
                // before the link, so it is the same object across reconnects.
                if (ConsumerBase *existing{m_consumerFacades.value(key)}) {
                    existing->setReplica(replica);
                    return;
                }
                // No facade for this contract: the raw dynamic Replica takes the name,
                // again on every reconnect, since the replaced one is retired above. A
                // framework point takes none; its C++ adopts it through
                // consumedReplicaReady.
                if (!connectPoint.framework && m_engine) {
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
