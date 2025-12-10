// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "topology.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSslCertificate>
#include <QSslKey>

namespace SynQt {

QList<ConnectPointConfig> Topology::owned() const
{
    QList<ConnectPointConfig> result;
    for (const ConnectPointConfig &connectPoint : connectPoints) {
        if (connectPoint.owner == entity) {
            result.append(connectPoint);
        }
    }
    return result;
}

QList<ConnectPointConfig> Topology::consumed() const
{
    QList<ConnectPointConfig> result;
    for (const ConnectPointConfig &connectPoint : connectPoints) {
        if (connectPoint.consumers.contains(entity)) {
            result.append(connectPoint);
        }
    }
    return result;
}

namespace {

MeshTransportMode transportModeFromString(const QString &value)
{
    return value == QLatin1String("local") ? MeshTransportMode::LocalSocket
                                            : MeshTransportMode::MutualTls;
}

} // namespace

Topology topologyFromJson(const QJsonObject &object)
{
    Topology topology;
    topology.entity = object.value(QStringLiteral("entity")).toString();
    // The entity's own answer to "one of you, or one per caller", defaulting to shared the
    // way the configuration does. Copied onto every point below, because the host of a
    // point is what acts on it.
    const bool shared{object.value(QStringLiteral("shared")).toBool(true)};
    topology.shared = shared;

    const QJsonObject credentials{object.value(QStringLiteral("credentials")).toObject()};
    topology.credentials.caCertPath = credentials.value(QStringLiteral("ca")).toString();
    topology.credentials.certPath = credentials.value(QStringLiteral("cert")).toString();
    topology.credentials.keyPath = credentials.value(QStringLiteral("key")).toString();

    topology.type = object.value(QStringLiteral("type")).toString();
    // The provider block for the type. The external `provider` object, or the embedded
    // `settings` object (sqlite) when no external provider is named.
    const QJsonObject provider{object.value(QStringLiteral("provider")).toObject()};
    if (!provider.isEmpty()) {
        topology.provider = provider.toVariantMap();
    } else {
        topology.provider = object.value(QStringLiteral("settings")).toObject().toVariantMap();
    }
    // The three QJsonArray locals below are not brace-initialized. QJsonArray has an
    // initializer_list constructor and a QJsonArray converts implicitly to QJsonValue,
    // so `QJsonArray steps{someArray}` builds an array holding that array as its single
    // element instead of copying it, and every entry then reads back empty. Copy
    // initialization picks the copy constructor. Same trap, same fix, as
    // Router::applyRemoteRouteTable.
    const QJsonArray schema = object.value(QStringLiteral("schema")).toArray();
    for (const QJsonValue &step : schema) {
        topology.schema.append(step.toString());
    }

    // network.outbound: absent or empty leaves this entity closed, which is the default
    // for every type. Copy initialization for the same reason as the arrays above.
    const QJsonObject network = object.value(QStringLiteral("network")).toObject();
    topology.outboundDeclared = network.contains(QStringLiteral("outbound"));
    const QJsonArray outbound = network.value(QStringLiteral("outbound")).toArray();
    for (const QJsonValue &value : outbound) {
        OutboundEndpoint endpoint;
        // Two spellings, one meaning. A bare string is a prefix and nothing else, which is
        // every entry that needs no key. An object adds a name to call it by and the
        // headers to send. Writing the short form must never be the reason a project loses
        // a capability, so both land here as the same record.
        if (value.isString()) {
            endpoint.url = value.toString();
        } else {
            const QJsonObject entry{value.toObject()};
            endpoint.name = entry.value(QStringLiteral("name")).toString();
            endpoint.url = entry.value(QStringLiteral("url")).toString();
            const QJsonObject headers{entry.value(QStringLiteral("headers")).toObject()};
            for (auto it{headers.constBegin()}; it != headers.constEnd(); ++it) {
                endpoint.headers.insert(it.key(), it.value().toString());
            }
        }
        topology.outbound.append(endpoint);
    }

    const QJsonArray connectPoints = object.value(QStringLiteral("connect_points")).toArray();
    for (const QJsonValue &value : connectPoints) {
        const QJsonObject entry{value.toObject()};
        ConnectPointConfig connectPoint;
        connectPoint.name = entry.value(QStringLiteral("name")).toString();
        connectPoint.contract = entry.value(QStringLiteral("contract")).toString();
        connectPoint.owner = entry.value(QStringLiteral("owner")).toString();
        connectPoint.serverFile = entry.value(QStringLiteral("server")).toString();
        connectPoint.framework = entry.value(QStringLiteral("framework")).toBool();
        connectPoint.shared = shared;
        const QJsonArray consumers = entry.value(QStringLiteral("consumers")).toArray();
        for (const QJsonValue &consumer : consumers) {
            connectPoint.consumers.append(consumer.toString());
        }
        const QJsonObject endpoint{entry.value(QStringLiteral("endpoint")).toObject()};
        connectPoint.endpoint.mode =
            transportModeFromString(endpoint.value(QStringLiteral("transport")).toString());
        connectPoint.endpoint.host =
            endpoint.value(QStringLiteral("host")).toString(QStringLiteral("127.0.0.1"));
        connectPoint.endpoint.port =
            static_cast<quint16>(endpoint.value(QStringLiteral("port")).toInt());
        connectPoint.endpoint.socketName = endpoint.value(QStringLiteral("socket")).toString();
        topology.connectPoints.append(connectPoint);
    }
    return topology;
}

QSslCertificate loadCertificate(const QString &path)
{
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return QSslCertificate{};
    }
    return QSslCertificate{file.readAll(), QSsl::Pem};
}

QSslKey loadPrivateKey(const QString &path)
{
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return QSslKey{};
    }
    return QSslKey{file.readAll(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey};
}

} // namespace SynQt
