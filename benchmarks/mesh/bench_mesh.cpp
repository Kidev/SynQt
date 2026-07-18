// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The mesh-transport baseline (M3): the service-to-service links, measured the way the
// transport benchmark measures the browser link. It stands up the real framework transports
// in one process (SynQt::MeshServer / MeshClient) and reports the full distribution
// (p50/p95/p99) for two link modes:
//
//   * mutual TLS on loopback; the DEFAULT for every mesh link, including two entities on one
//     host (the same QSslServer/QSslSocket pair, just bound to the loopback interface);
//   * the opt-in local socket (QLocalServer/QLocalSocket); the explicit fast path.
//
// The point of the run is the DELTA between them. The benchmarking plan is explicit that the
// loopback-mTLS vs local-socket gap is "the number that justifies keeping transport: local as
// an explicit fast path; measure it, do not assume it." So this harness measures, per link:
//   connection setup cost (handshake + verify against the project CA vs a local connect),
//   slot round-trip latency, one-way property-push propagation, and pipelined throughput.
//
// Cross-host mutual TLS cannot be stood up in a single process; its steady-state per-message
// cost is the loopback-mTLS number plus real network latency, and its setup cost the
// loopback-mTLS handshake plus one network RTT. The loopback figures here are the floor.

#include "rep_bench_mesh_source.h"
#include "rep_bench_mesh_replica.h"

#include "meshclient.h"
#include "meshpeer.h"
#include "meshserver.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCallWatcher>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QString>
#include <QSysInfo>
#include <QTextStream>
#include <QUrl>

#include <algorithm>
#include <cmath>

using SynQt::MeshClient;
using SynQt::MeshPeer;
using SynQt::MeshServer;

namespace {

// The Source under test: roundTrip echoes the payload back (a returning slot), so the
// consumer can time a full consumer -> owner -> reply cycle across the mesh link.
class MeshBenchBackend : public MeshBenchSimpleSource
{
    Q_OBJECT

public:
    using MeshBenchSimpleSource::MeshBenchSimpleSource;

    QByteArray roundTrip(QByteArray payload) override
    {
        return payload;
    }
};

// One measured distribution, in milliseconds; the same percentile scaffolding the transport
// benchmark uses, so the two baselines read identically.
struct Distribution
{
    QString name;
    QString unit{QStringLiteral("ms")};
    QList<double> samples;

    double percentile(double fraction) const
    {
        if (samples.isEmpty()) {
            return 0.0;
        }
        QList<double> sorted{samples};
        std::sort(sorted.begin(), sorted.end());
        const double rank{fraction * (sorted.size() - 1)};
        const int low{int(std::floor(rank))};
        const int high{int(std::ceil(rank))};
        if (low == high) {
            return sorted.at(low);
        }
        return sorted.at(low) + (rank - low) * (sorted.at(high) - sorted.at(low));
    }

    double mean() const
    {
        if (samples.isEmpty()) {
            return 0.0;
        }
        double total{0.0};
        for (const double value : samples) {
            total += value;
        }
        return total / samples.size();
    }

    double min() const
    {
        return samples.isEmpty() ? 0.0
                                 : *std::min_element(samples.constBegin(), samples.constEnd());
    }
    double max() const
    {
        return samples.isEmpty() ? 0.0
                                 : *std::max_element(samples.constBegin(), samples.constEnd());
    }

    QJsonObject toJson() const
    {
        return QJsonObject{{QStringLiteral("name"), name},
                           {QStringLiteral("unit"), unit},
                           {QStringLiteral("samples"), samples.size()},
                           {QStringLiteral("min"), min()},
                           {QStringLiteral("p50"), percentile(0.50)},
                           {QStringLiteral("p95"), percentile(0.95)},
                           {QStringLiteral("p99"), percentile(0.99)},
                           {QStringLiteral("max"), max()},
                           {QStringLiteral("mean"), mean()}};
    }
};

// One scalar result (throughput and the like) that is not a distribution.
struct Scalar
{
    QString name;
    QString unit;
    double value{0.0};

    QJsonObject toJson() const
    {
        return QJsonObject{{QStringLiteral("name"), name},
                           {QStringLiteral("unit"), unit},
                           {QStringLiteral("value"), value}};
    }
};

template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!predicate()) {
        if (clock.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

QByteArray readAll(const QString &path)
{
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray{};
    }
    return file.readAll();
}

QString certPath(const QString &name, const char *extension)
{
    return QString::fromUtf8(MESH_BENCH_CERT_DIR) + QLatin1Char('/') + name
           + QLatin1String(extension);
}

QSslCertificate loadCert(const QString &name)
{
    return QSslCertificate{readAll(certPath(name, ".crt")), QSsl::Pem};
}

QSslKey loadKey(const QString &name)
{
    return QSslKey{readAll(certPath(name, ".key")), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey};
}

enum class LinkKind { MutualTls, LocalSocket };

QString linkLabel(LinkKind kind)
{
    return kind == LinkKind::MutualTls ? QStringLiteral("mtls_loopback")
                                       : QStringLiteral("local_socket");
}

// A live, established mesh link: owner (server + QtRO host + Source) and consumer (client +
// QtRO node + replica), kept alive for the whole steady-state measurement.
struct Link
{
    MeshServer server;
    QRemoteObjectHost host;
    MeshBenchBackend source;
    QRemoteObjectNode node;
    MeshClient client;
    QScopedPointer<MeshBenchReplica> replica;
    bool ready{false};
};

// Bring up one link of the given kind and acquire its replica. `index` disambiguates the
// QtRO host URL and the local socket name across the two links in the run.
void establish(Link &link, LinkKind kind, int index)
{
    link.host.setHostUrl(QUrl{QStringLiteral("synqt-bench-mesh-%1:///host").arg(index)},
                         QRemoteObjectHost::AllowExternalRegistration);
    if (!link.host.enableRemoting<MeshBenchSourceAPI>(&link.source)) {
        qCritical("bench-mesh: enableRemoting failed");
        return;
    }

    QObject::connect(&link.server, &MeshServer::peerConnected, &link.host,
                     [&link](QIODevice *device, const MeshPeer &) {
                         link.host.addHostSideConnection(device);
                     });
    QObject::connect(&link.client, &MeshClient::connected, &link.node,
                     [&link](QIODevice *device) { link.node.addClientSideConnection(device); });

    bool listening{false};
    bool connecting{false};
    if (kind == LinkKind::MutualTls) {
        const QSslCertificate ca{loadCert(QStringLiteral("ca"))};
        listening = link.server.listenMutualTls(QHostAddress::LocalHost, 0, ca,
                                                 loadCert(QStringLiteral("alpha")),
                                                 loadKey(QStringLiteral("alpha")));
        if (listening) {
            connecting = link.client.connectMutualTls(QHostAddress::LocalHost,
                                                       link.server.serverPort(),
                                                       QStringLiteral("alpha"), ca,
                                                       loadCert(QStringLiteral("beta")),
                                                       loadKey(QStringLiteral("beta")));
        }
    } else {
        const QString socketName{
            QStringLiteral("synqt-bench-mesh-%1-%2")
                .arg(QCoreApplication::applicationPid())
                .arg(index)};
        listening = link.server.listenLocal(socketName, QStringLiteral("beta"));
        if (listening) {
            connecting = link.client.connectLocal(socketName);
        }
    }
    if (!listening || !connecting) {
        qCritical("bench-mesh: link setup failed: %s", qPrintable(link.server.errorString()));
        return;
    }

    link.replica.reset(link.node.acquire<MeshBenchReplica>());
    link.ready = link.replica->waitForSource(10000);
    if (!link.ready) {
        qCritical("bench-mesh: replica never became ready on the %s link",
                  qPrintable(linkLabel(kind)));
    }
}

double oneRoundTrip(MeshBenchReplica *replica, const QByteArray &payload)
{
    QElapsedTimer clock;
    clock.start();
    QRemoteObjectPendingReply<QByteArray> reply{replica->roundTrip(payload)};
    reply.waitForFinished();
    return clock.nsecsElapsed() / 1.0e6;
}

Distribution measureRoundTrip(LinkKind kind, MeshBenchReplica *replica, int samples,
                              int payloadBytes)
{
    const QByteArray payload(payloadBytes, 'x');
    Distribution distribution;
    distribution.name =
        QStringLiteral("%1.slot_round_trip_%2B").arg(linkLabel(kind)).arg(payloadBytes);
    distribution.samples.reserve(samples);
    for (int i{0}; i < samples; ++i) {
        distribution.samples.append(oneRoundTrip(replica, payload));
    }
    return distribution;
}

Distribution measurePropertyPush(LinkKind kind, MeshBenchBackend *source,
                                 MeshBenchReplica *replica, int samples)
{
    Distribution distribution;
    distribution.name = QStringLiteral("%1.property_push_propagation").arg(linkLabel(kind));
    distribution.samples.reserve(samples);
    for (int i{1}; i <= samples; ++i) {
        const qint64 stamp{i};
        QElapsedTimer clock;
        clock.start();
        source->setTick(stamp);
        const bool arrived{spinUntil([&]() { return replica->tick() == stamp; }, 5000)};
        if (arrived) {
            distribution.samples.append(clock.nsecsElapsed() / 1.0e6);
        }
    }
    return distribution;
}

Scalar measureThroughput(LinkKind kind, MeshBenchReplica *replica, int count, int payloadBytes)
{
    const QByteArray payload(payloadBytes, 'y');
    int finished{0};
    QElapsedTimer clock;
    clock.start();
    for (int i{0}; i < count; ++i) {
        QRemoteObjectPendingReply<QByteArray> reply{replica->roundTrip(payload)};
        auto *watcher{new QRemoteObjectPendingCallWatcher{reply}};
        QObject::connect(watcher, &QRemoteObjectPendingCallWatcher::finished, replica,
                         [&finished, watcher](QRemoteObjectPendingCallWatcher *) {
                             ++finished;
                             watcher->deleteLater();
                         });
    }
    spinUntil([&]() { return finished >= count; }, 60000);
    const double seconds{clock.nsecsElapsed() / 1.0e9};
    Scalar scalar;
    scalar.name = QStringLiteral("%1.slot_throughput_%2B").arg(linkLabel(kind)).arg(payloadBytes);
    scalar.unit = QStringLiteral("calls/s");
    scalar.value = seconds > 0.0 ? count / seconds : 0.0;
    return scalar;
}

// Connection setup cost: for each sample, bring up a fresh link, time from the connect call
// until the transport is up (mutual TLS: the handshake plus the peer-certificate verify
// against the project CA has completed and MeshServer emitted peerConnected; local socket: the
// connect completed). This isolates the per-connection cost the two modes differ on; it does
// NOT include the QtRO API-definition exchange, which is transport-independent.
Distribution measureSetupCost(LinkKind kind, int samples)
{
    Distribution distribution;
    distribution.name = QStringLiteral("%1.connection_setup").arg(linkLabel(kind));
    distribution.samples.reserve(samples);

    const QSslCertificate ca{loadCert(QStringLiteral("ca"))};
    const QSslCertificate alphaCert{loadCert(QStringLiteral("alpha"))};
    const QSslKey alphaKey{loadKey(QStringLiteral("alpha"))};
    const QSslCertificate betaCert{loadCert(QStringLiteral("beta"))};
    const QSslKey betaKey{loadKey(QStringLiteral("beta"))};

    for (int i{0}; i < samples; ++i) {
        MeshServer server;
        // The owner does not need a QtRO host for a pure setup-cost measurement: just count a
        // verified peer and drop its transport, so the run does not accumulate connections.
        int accepted{0};
        QObject::connect(&server, &MeshServer::peerConnected, &server,
                         [&accepted](QIODevice *device, const MeshPeer &) {
                             ++accepted;
                             device->deleteLater();
                         });

        bool up{false};
        if (kind == LinkKind::MutualTls) {
            if (!server.listenMutualTls(QHostAddress::LocalHost, 0, ca, alphaCert, alphaKey)) {
                continue;
            }
        } else {
            const QString socketName{QStringLiteral("synqt-bench-setup-%1-%2")
                                         .arg(QCoreApplication::applicationPid())
                                         .arg(i)};
            if (!server.listenLocal(socketName, QStringLiteral("beta"))) {
                continue;
            }
            MeshClient client;
            bool connected{false};
            QObject::connect(&client, &MeshClient::connected, &client,
                             [&connected](QIODevice *) { connected = true; });
            QElapsedTimer clock;
            clock.start();
            if (!client.connectLocal(socketName)) {
                continue;
            }
            up = spinUntil([&]() { return connected && accepted >= 1; }, 5000);
            if (up) {
                distribution.samples.append(clock.nsecsElapsed() / 1.0e6);
            }
            continue;
        }

        // Mutual TLS path: time the client handshake + verify.
        MeshClient client;
        bool connected{false};
        QObject::connect(&client, &MeshClient::connected, &client,
                         [&connected](QIODevice *) { connected = true; });
        QElapsedTimer clock;
        clock.start();
        if (!client.connectMutualTls(QHostAddress::LocalHost, server.serverPort(),
                                     QStringLiteral("alpha"), ca, betaCert, betaKey)) {
            continue;
        }
        up = spinUntil([&]() { return connected && accepted >= 1; }, 5000);
        if (up) {
            distribution.samples.append(clock.nsecsElapsed() / 1.0e6);
        }
    }
    return distribution;
}

void printDistribution(QTextStream &out, const Distribution &distribution)
{
    out << qSetFieldWidth(38) << Qt::left << distribution.name << qSetFieldWidth(0) << Qt::right
        << "  n=" << distribution.samples.size()
        << "  p50=" << QString::number(distribution.percentile(0.50), 'f', 3)
        << "  p95=" << QString::number(distribution.percentile(0.95), 'f', 3)
        << "  p99=" << QString::number(distribution.percentile(0.99), 'f', 3)
        << "  max=" << QString::number(distribution.max(), 'f', 3) << " " << distribution.unit
        << Qt::endl;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption samplesOption{QStringLiteral("samples"),
        QStringLiteral("Latency samples per distribution."), QStringLiteral("n"),
        QStringLiteral("2000")};
    const QCommandLineOption setupOption{QStringLiteral("setup-samples"),
        QStringLiteral("Connection-setup samples per link mode."), QStringLiteral("n"),
        QStringLiteral("200")};
    const QCommandLineOption warmupOption{QStringLiteral("warmup"),
        QStringLiteral("Warm-up round trips per link before measuring."), QStringLiteral("n"),
        QStringLiteral("500")};
    const QCommandLineOption throughputOption{QStringLiteral("throughput-calls"),
        QStringLiteral("Pipelined calls for the throughput figure."), QStringLiteral("n"),
        QStringLiteral("20000")};
    const QCommandLineOption outOption{QStringLiteral("out"),
        QStringLiteral("JSON baseline output path."), QStringLiteral("file")};
    parser.addOptions({samplesOption, setupOption, warmupOption, throughputOption, outOption});
    parser.process(app);

    const int samples{parser.value(samplesOption).toInt()};
    const int setupSamples{parser.value(setupOption).toInt()};
    const int warmup{parser.value(warmupOption).toInt()};
    const int throughputCalls{parser.value(throughputOption).toInt()};

    if (!QSslSocket::supportsSsl()) {
        qCritical("bench-mesh: no TLS backend; cannot measure the mutual-TLS link");
        return 1;
    }
    if (loadCert(QStringLiteral("ca")).isNull()) {
        qCritical("bench-mesh: certificates missing under %s; run gen-certs.sh",
                  MESH_BENCH_CERT_DIR);
        return 1;
    }

    QTextStream out{stdout};
    out << "SynQt mesh-transport baseline (SynQt::MeshServer/MeshClient, one process)" << Qt::endl;
    out << "Qt " << qVersion() << " on " << QSysInfo::prettyProductName() << " ("
        << QSysInfo::currentCpuArchitecture() << ")" << Qt::endl;
    out << "warmup=" << warmup << " samples=" << samples << " setup-samples=" << setupSamples
        << " throughput-calls=" << throughputCalls << Qt::endl
        << Qt::endl;

    QList<Distribution> distributions;
    QList<Scalar> scalars;

    // Steady-state measurements, one established link per mode.
    int index{0};
    for (const LinkKind kind : {LinkKind::MutualTls, LinkKind::LocalSocket}) {
        Link link;
        establish(link, kind, index++);
        if (!link.ready) {
            out << linkLabel(kind) << ": link did not come up (skipped)" << Qt::endl;
            continue;
        }
        const QByteArray warmupPayload(64, 'w');
        for (int i{0}; i < warmup; ++i) {
            oneRoundTrip(link.replica.data(), warmupPayload);
        }
        distributions.append(measureRoundTrip(kind, link.replica.data(), samples, 64));
        distributions.append(measureRoundTrip(kind, link.replica.data(), samples, 4096));
        distributions.append(
            measurePropertyPush(kind, &link.source, link.replica.data(), samples));
        scalars.append(measureThroughput(kind, link.replica.data(), throughputCalls, 64));
    }

    // Connection-setup cost, the number the transport: local fast path is justified by.
    for (const LinkKind kind : {LinkKind::MutualTls, LinkKind::LocalSocket}) {
        distributions.append(measureSetupCost(kind, setupSamples));
    }

    for (const Distribution &distribution : distributions) {
        printDistribution(out, distribution);
    }
    for (const Scalar &scalar : scalars) {
        out << qSetFieldWidth(38) << Qt::left << scalar.name << qSetFieldWidth(0) << Qt::right
            << "  " << QString::number(scalar.value, 'f', 0) << " " << scalar.unit << Qt::endl;
    }

    // The delta that justifies keeping the local socket as an explicit fast path.
    auto setupP50 = [&distributions](LinkKind kind) -> double {
        const QString wanted{QStringLiteral("%1.connection_setup").arg(linkLabel(kind))};
        for (const Distribution &distribution : distributions) {
            if (distribution.name == wanted) {
                return distribution.percentile(0.50);
            }
        }
        return 0.0;
    };
    const double tlsSetup{setupP50(LinkKind::MutualTls)};
    const double localSetup{setupP50(LinkKind::LocalSocket)};
    out << Qt::endl
        << "connection-setup delta (mtls_loopback / local_socket p50): "
        << QString::number(tlsSetup, 'f', 3) << " / " << QString::number(localSetup, 'f', 3)
        << " ms";
    if (localSetup > 0.0) {
        out << "  (" << QString::number(tlsSetup / localSetup, 'f', 1) << "x)";
    }
    out << Qt::endl;

    // Write the JSON baseline.
    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("mesh"));
    root.insert(QStringLiteral("path"), QStringLiteral("synqt-mesh-transports-one-process"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("warmup"), warmup);
    root.insert(QStringLiteral("samples"), samples);
    root.insert(QStringLiteral("setup_samples"), setupSamples);
    QJsonArray distJson;
    for (const Distribution &distribution : distributions) {
        distJson.append(distribution.toJson());
    }
    root.insert(QStringLiteral("latency"), distJson);
    QJsonArray scalarJson;
    for (const Scalar &scalar : scalars) {
        scalarJson.append(scalar.toJson());
    }
    root.insert(QStringLiteral("throughput"), scalarJson);

    const QString outPath{parser.value(outOption)};
    if (!outPath.isEmpty()) {
        QFile file{outPath};
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
            file.close();
            out << Qt::endl << "wrote baseline " << QDir{outPath}.absolutePath() << Qt::endl;
        } else {
            qWarning("bench-mesh: cannot write %s", qPrintable(outPath));
        }
    }
    return 0;
}

#include "bench_mesh.moc"
