// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "entityruntime.h"

#include "connectpointhost.h"
#include "meshclient.h"

#include "consumerbase.h"
#include "consumerfactory.h"

#include "cache.h"
#include "cachefactory.h"
#include "db.h"
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

bool EntityRuntime::buildBlueprintContext()
{
    const QString blueprint{m_topology.blueprint};
    if (blueprint == QLatin1String("persistence")) {
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
        // A schema that did not apply is fatal: every Source on this entity is written
        // against it, so starting would only move the failure to the first query.
        if (!m_topology.schema.isEmpty() && !m_persistence->migrate(m_topology.schema, &error)) {
            m_errorString = error;
            m_persistence.reset();
            return false;
        }
        m_blueprintContext.insert(QStringLiteral("Db"), new Db{m_persistence.get(), this});
    } else if (blueprint == QLatin1String("cache")) {
        m_cache = makeCacheProvider(providerConfigFromMap(m_topology.provider), &m_errorString);
        if (m_cache == nullptr) {
            return false;
        }
        // Unlike a database, an unreachable cache is not fatal: a cache miss is a normal
        // outcome, the provider reports isHealthy(), and an external engine may come up
        // after the entity does. It is still said out loud, never swallowed.
        QString error;
        if (!m_cache->connect(&error)) {
            qWarning("SynQt: cache provider '%s' is not connected: %s",
                     qUtf8Printable(m_cache->name()), qUtf8Printable(error));
        }
        m_blueprintContext.insert(QStringLiteral("Cache"), new Cache{m_cache.get(), this});
    } else if (blueprint == QLatin1String("document")) {
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
        // The document family has no dedicated QML helper yet; the provider is held so a
        // custom Source (or a future Docs helper) can reach it.
    } else if (blueprint == QLatin1String("gateway")) {
        m_network = new QNetworkAccessManager{this};
        const bool release{m_topology.provider.value(QStringLiteral("release"), true).toBool()};
        m_blueprintContext.insert(QStringLiteral("Http"),
                                  new Http{m_network, m_engine, release, this});
    } else if (blueprint == QLatin1String("jobs")) {
        m_blueprintContext.insert(QStringLiteral("Jobs"), new Jobs{1000, this});
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

QList<ConnectPointHost *> EntityRuntime::ownedHosts() const
{
    return m_ownedHosts;
}

QQmlPropertyMap *EntityRuntime::accessorFor(const QString &capitalizedOwner)
{
    if (QQmlPropertyMap *existing{m_accessors.value(capitalizedOwner)}) {
        return existing;
    }
    QQmlPropertyMap *map{QQmlPropertyMap::create(this)};
    m_accessors.insert(capitalizedOwner, map);
    if (m_engine) {
        m_engine->rootContext()->setContextProperty(capitalizedOwner, map);
    }
    return map;
}

QQmlPropertyMap *EntityRuntime::accessor(const QString &capitalizedOwner) const
{
    return m_accessors.value(capitalizedOwner);
}

QObject *EntityRuntime::consumedReplica(const QString &owner, const QString &connectPoint) const
{
    return m_consumedReplicas.value(owner + QLatin1Char('/') + connectPoint);
}

bool EntityRuntime::start()
{
    // Build the blueprint backend once, so every owned Source is created with its helper
    // (Db/Cache/Http/Jobs) already in context (a shared Source is created inside start()).
    // An entity that cannot serve its blueprint never reaches enableRemoting(): a consumer
    // being refused acquisition is a far better failure than one acquiring a Source whose
    // every call will fail.
    if (!buildBlueprintContext()) {
        return false;
    }

    // Bring up an owner for every connect point this entity owns.
    for (const ConnectPointConfig &connectPoint : m_topology.owned()) {
        ConnectPointHost *host{
            new ConnectPointHost{connectPoint, m_topology.credentials, m_engine, this}};
        for (auto it{m_blueprintContext.constBegin()}; it != m_blueprintContext.constEnd(); ++it) {
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

    // Open a consumer link for every connect point this entity consumes; and only
    // those (deny by default: no link to an owner this entity does not consume from).
    for (const ConnectPointConfig &connectPoint : m_topology.consumed()) {
        openConsumerLink(connectPoint);
    }
    return true;
}

void EntityRuntime::openConsumerLink(const ConnectPointConfig &connectPoint)
{
    MeshClient *client{new MeshClient{this}};

    connect(client, &MeshClient::connected, this,
            [this, connectPoint](QIODevice *device) {
                QRemoteObjectNode *node{new QRemoteObjectNode{this}};
                node->addClientSideConnection(device);
                node->setHeartbeatInterval(1000);
                QRemoteObjectDynamicReplica *replica{node->acquireDynamic(connectPoint.name)};
                replica->setParent(node);
                const QString key{connectPoint.owner + QLatin1Char('/') + connectPoint.name};
                m_consumedReplicas.insert(key, replica);
                QQmlPropertyMap *map{accessorFor(accessorName(connectPoint.owner))};

                // Expose the consumer facade (<Owner>.<name>) when the contract's consumer
                // surface is registered, so a service reaches another entity through the
                // same ergonomic surface as the client (returning-slot promises,
                // `<Contract>.on<Signal>`). Stable across reconnects; falls back to the raw
                // dynamic Replica when no facade is registered.
                if (ConsumerBase *existing{m_consumerFacades.value(key)}) {
                    existing->setReplica(replica);
                    return;
                }
                ConsumerBase *facade{makeConsumer(connectPoint.contract)};
                if (facade != nullptr) {
                    facade->setPoint(connectPoint.name);
                    facade->setParent(this);
                    m_consumerFacades.insert(key, facade);
                    map->insert(connectPoint.name, QVariant::fromValue<QObject *>(facade));
                    facade->setReplica(replica);
                    return;
                }
                map->insert(connectPoint.name, QVariant::fromValue<QObject *>(replica));
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
