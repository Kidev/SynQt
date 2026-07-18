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

ConnectPointInstance instanceFromString(const QString &value)
{
    if (value == QLatin1String("per_session")) {
        return ConnectPointInstance::PerSession;
    }
    if (value == QLatin1String("per_peer")) {
        return ConnectPointInstance::PerPeer;
    }
    return ConnectPointInstance::Shared;
}

} // namespace

Topology topologyFromJson(const QJsonObject &object)
{
    Topology topology;
    topology.entity = object.value(QStringLiteral("entity")).toString();

    const QJsonObject credentials{object.value(QStringLiteral("credentials")).toObject()};
    topology.credentials.caCertPath = credentials.value(QStringLiteral("ca")).toString();
    topology.credentials.certPath = credentials.value(QStringLiteral("cert")).toString();
    topology.credentials.keyPath = credentials.value(QStringLiteral("key")).toString();

    topology.blueprint = object.value(QStringLiteral("blueprint")).toString();
    // The provider block for the blueprint: the external `provider` object, or the embedded
    // `settings` object (sqlite) when no external provider is named.
    const QJsonObject provider{object.value(QStringLiteral("provider")).toObject()};
    if (!provider.isEmpty()) {
        topology.provider = provider.toVariantMap();
    } else {
        topology.provider = object.value(QStringLiteral("settings")).toObject().toVariantMap();
    }
    const QJsonArray schema{object.value(QStringLiteral("schema")).toArray()};
    for (const QJsonValue &step : schema) {
        topology.schema.append(step.toString());
    }

    const QJsonArray connectPoints{object.value(QStringLiteral("connect_points")).toArray()};
    for (const QJsonValue &value : connectPoints) {
        const QJsonObject entry{value.toObject()};
        ConnectPointConfig connectPoint;
        connectPoint.name = entry.value(QStringLiteral("name")).toString();
        connectPoint.contract = entry.value(QStringLiteral("contract")).toString();
        connectPoint.owner = entry.value(QStringLiteral("owner")).toString();
        connectPoint.serverFile = entry.value(QStringLiteral("server")).toString();
        connectPoint.instance =
            instanceFromString(entry.value(QStringLiteral("instance")).toString());
        const QJsonArray consumers{entry.value(QStringLiteral("consumers")).toArray()};
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
