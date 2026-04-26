// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "topology.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>

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
    // The entity's own shared setting, shared by default like the configuration, copied
    // onto every point below since the host of a point acts on it.
    const bool shared{object.value(QStringLiteral("shared")).toBool(true)};
    topology.shared = shared;

    const QJsonObject credentials{object.value(QStringLiteral("credentials")).toObject()};
    topology.credentials.caCertPath = credentials.value(QStringLiteral("ca")).toString();
    topology.credentials.certPath = credentials.value(QStringLiteral("cert")).toString();
    topology.credentials.keyPath = credentials.value(QStringLiteral("key")).toString();

    topology.type = object.value(QStringLiteral("type")).toString();
    // The type's provider block: the external `provider` object, or the embedded `settings`
    // object (sqlite) when no provider is named.
    const QJsonObject provider{object.value(QStringLiteral("provider")).toObject()};
    if (!provider.isEmpty()) {
        topology.provider = provider.toVariantMap();
    } else {
        topology.provider = object.value(QStringLiteral("settings")).toObject().toVariantMap();
    }
    // The three QJsonArray locals below are copy-initialized, not braced: QJsonArray has an
    // initializer_list constructor and converts to QJsonValue, so `QJsonArray
    // steps{someArray}` would hold the array as one element. Same as
    // Router::applyRemoteRouteTable.
    const QJsonArray schema = object.value(QStringLiteral("schema")).toArray();
    for (const QJsonValue &step : schema) {
        topology.schema.append(step.toString());
    }

    // Where this entity spools what a monitor did not take, inside its own build directory.
    const QJsonObject monitoring = object.value(QStringLiteral("monitoring")).toObject();
    topology.spoolDir = monitoring.value(QStringLiteral("spool_dir")).toString();
    if (monitoring.contains(QStringLiteral("spool_cap_bytes"))) {
        topology.spoolCapBytes =
            static_cast<qint64>(monitoring.value(QStringLiteral("spool_cap_bytes")).toDouble());
    }
    // Per-category levels, read at startup so an operator can change them without a
    // rebuild.
    const QJsonObject levels = monitoring.value(QStringLiteral("levels")).toObject();
    for (auto it{levels.constBegin()}; it != levels.constEnd(); ++it) {
        topology.traceLevels.insert(it.key(), it.value().toString());
    }

    // network.outbound. Absent or empty leaves the entity closed (the default).
    // Copy-initialized as above.
    const QJsonObject network = object.value(QStringLiteral("network")).toObject();
    topology.outboundDeclared = network.contains(QStringLiteral("outbound"));
    const QJsonArray outbound = network.value(QStringLiteral("outbound")).toArray();
    for (const QJsonValue &value : outbound) {
        OutboundEndpoint endpoint;
        // A bare string is a prefix only; an object adds a name and headers. Both become
        // the same record.
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

namespace {

/// The bytes of a PEM file, or nothing, logging which file failed. Both callers turn an
/// unreadable file into a null object, and Qt does not complain about a null certificate or
/// key: the server listens and fails every handshake.
QByteArray pemBytes(const QString &path, const char *what)
{
    if (path.isEmpty()) {
        return QByteArray{};  // nothing was configured. The caller decides whether that is an error
    }
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("SynQt: cannot read the %s at %s: %s", what, qUtf8Printable(path),
                 qUtf8Printable(file.errorString()));
        return QByteArray{};
    }
    return file.readAll();
}

/// What to call \a algorithm in a sentence an operator reads.
QString algorithmName(QSsl::KeyAlgorithm algorithm)
{
    switch (algorithm) {
    case QSsl::Rsa:
        return QStringLiteral("an RSA");
    case QSsl::Dsa:
        return QStringLiteral("a DSA");
    case QSsl::Ec:
        return QStringLiteral("an elliptic-curve");
    case QSsl::Dh:
        return QStringLiteral("a Diffie-Hellman");
    case QSsl::MlDsa:
        return QStringLiteral("an ML-DSA");
    case QSsl::Opaque:
        break;
    }
    return QStringLiteral("this");
}

} // namespace

QSslCertificate loadCertificate(const QString &path)
{
    const QByteArray pem{pemBytes(path, "certificate")};
    if (pem.isEmpty()) {
        return QSslCertificate{};
    }
    const QSslCertificate certificate{pem, QSsl::Pem};
    if (certificate.isNull()) {
        qWarning("SynQt: %s holds no PEM certificate", qUtf8Printable(path));
    }
    return certificate;
}

QSslKey loadPrivateKey(const QString &path)
{
    const QByteArray pem{pemBytes(path, "private key")};
    if (pem.isEmpty()) {
        return QSslKey{};
    }
    // Each algorithm in turn: QSslKey decodes only the algorithm it is given and returns a
    // null key otherwise. An EC key (what an ACME client writes for `--key-type ecdsa`)
    // would otherwise load as nothing.
    for (const QSsl::KeyAlgorithm algorithm :
         {QSsl::Rsa, QSsl::Ec, QSsl::Dsa, QSsl::Dh, QSsl::MlDsa}) {
        const QSslKey key{pem, algorithm, QSsl::Pem, QSsl::PrivateKey};
        if (!key.isNull()) {
            return key;
        }
    }
    qWarning("SynQt: %s holds no PEM private key this build can read. An encrypted key has "
             "to be decrypted before an entity is given it, and a key of an algorithm this "
             "Qt was built without cannot be read at all.", qUtf8Printable(path));
    return QSslKey{};
}

QString unusableKeyReason(const QSslKey &key)
{
    if (key.isNull()) {
        return QStringLiteral("there is no key to present");
    }
    // What the PKCS#12 builder used by every non-OpenSSL backend can write. Qt cannot
    // report which key algorithms a backend supports (QSslSocket reports implemented
    // classes), so this pair is taken from qtbase's builder.
    if (key.algorithm() == QSsl::Rsa || key.algorithm() == QSsl::Dsa) {
        return QString{};
    }
    const QString backend{QSslSocket::activeBackend()};
    if (backend == QLatin1String("openssl")) {
        return QString{};
    }
    // The article is part of the name, since "an RSA" and "a DSA" do not share one.
    return QStringLiteral("%1 key cannot be presented by the %2 TLS backend this build "
                          "runs on, which carries an RSA or DSA key only; reissue the "
                          "certificate with an RSA key (certbot --key-type rsa) or run "
                          "against a Qt built with OpenSSL")
        .arg(algorithmName(key.algorithm()), backend);
}

} // namespace SynQt
