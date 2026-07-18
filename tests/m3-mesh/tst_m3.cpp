// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M3 acceptance: the service-to-service mesh transports.
//   - mutual TLS is the default on every link (loopback for same host): two native
//     nodes exchange a property and a slot, each verifying the other against the CA,
//     and the owner reads the caller's entity name from the verified certificate;
//   - a consumer presenting no certificate, or one from a different CA, is rejected at
//     the TLS handshake and never becomes an authenticated peer;
//   - the opt-in local socket exchanges a property and a slot, its peer trusted by
//     colocation (authenticated == false), never selected implicitly.

#include "rep_mesh_source.h"
#include "rep_mesh_replica.h"

#include "meshclient.h"
#include "meshpeer.h"
#include "meshserver.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslCertificateExtension>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTest>
#include <QUrl>

#include <cstdio>

using SynQt::MeshClient;
using SynQt::MeshPeer;
using SynQt::MeshServer;

namespace {

// A crash-safe progress trace. On Windows a piped standard stream is block-buffered, so a hard
// termination (an access violation, not a failed QVERIFY) discards everything QtTest printed and
// the failure reaches the ctest log as "***Failed" in half a second with no output at all -- not
// even QtTest's own "Start testing" banner, which is why unbuffering stdout alone did not reveal
// the crash site. This writes each step to a file that is reopened, flushed, and closed per line,
// so whatever is on disk after the process dies is exactly the last step it reached. QFile, not
// fopen: MSVC compiles this suite with /W4 /WX, where fopen is C4996 and would fail the build. The
// path is an absolute compile-time define, so the test's working directory does not matter.
void traceWrite(const QString &message, QIODevice::OpenMode mode)
{
#ifdef M3_TRACE_LOG
    QFile file{QString::fromUtf8(M3_TRACE_LOG)};
    if (!file.open(mode)) {
        return;
    }
    file.write(message.toUtf8());
    file.write("\n");
    file.close();
#else
    Q_UNUSED(message);
    Q_UNUSED(mode);
#endif
}

void traceMark(const QString &message)
{
    traceWrite(message, QIODevice::WriteOnly | QIODevice::Append);
}

// Start a fresh trace, discarding any file left by a previous local run.
void traceReset(const QString &message)
{
    traceWrite(message, QIODevice::WriteOnly | QIODevice::Truncate);
}

QByteArray readAll(const QString &path)
{
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray{};
    }
    return file.readAll();
}

QString assetPath(const QString &name, const char *extension)
{
    return QString::fromUtf8(M3_CERT_DIR) + QLatin1Char('/') + name + QLatin1String(extension);
}

QSslCertificate loadCert(const QString &name)
{
    return QSslCertificate{readAll(assetPath(name, ".crt")), QSsl::Pem};
}

QSslKey loadKey(const QString &name)
{
    return QSslKey{readAll(assetPath(name, ".key")), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey};
}

// What this host's TLS stack is, and what its openssl actually issued. Both are printed
// rather than assumed because both vary by host and neither is visible from a failed
// handshake: Qt picks its backend from what it can load at runtime (OpenSSL where it is
// available, the platform stack otherwise), and the certificates are generated on the host
// by whichever `openssl` is on PATH -- which on macOS is LibreSSL, not OpenSSL. A mutual-TLS
// test that fails on one platform and passes on another is unreadable without these two
// facts, and reading them out of a green run is how they stay trustworthy.
void reportTlsEnvironment()
{
    qInfo() << "TLS backend:" << QSslSocket::activeBackend()
            << "of" << QSslSocket::availableBackends()
            << "library:" << QSslSocket::sslLibraryVersionString();
    const QStringList names{QStringLiteral("ca"), QStringLiteral("alpha")};
    for (const QString &name : names) {
        const QSslCertificate certificate{loadCert(name)};
        if (certificate.isNull()) {
            qWarning() << "certificate" << name << "is missing or unreadable";
            continue;
        }
        QStringList extensions;
        const QList<QSslCertificateExtension> found{certificate.extensions()};
        for (const QSslCertificateExtension &extension : found) {
            // Rendered through QDebug rather than QVariant::toString(): an extension's value
            // is a map or a list as often as it is a string (basicConstraints and
            // extendedKeyUsage both are), and toString() renders those as nothing at all --
            // which prints the extension as present but empty, the one reading that makes a
            // correct certificate look like a broken one.
            QString rendered;
            QDebug{&rendered}.nospace() << extension.value();
            extensions << (extension.name() + QLatin1Char('=') + rendered.trimmed());
        }
        qInfo().noquote() << "certificate" << name
                          << "expires" << certificate.expiryDate().toString(Qt::ISODate)
                          << "extensions:" << extensions.join(QLatin1String("; "));
    }
}

} // namespace

// Concrete Source: records the last poke so the test can prove the slot arrived.
class MeshBackend : public MeshSimpleSource
{
    Q_OBJECT

public:
    using MeshSimpleSource::MeshSimpleSource;

    int lastPoke{-1};

    void poke(int n) override
    {
        lastPoke = n;
    }
};

class TestM3 : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        traceMark(QStringLiteral("enter initTestCase"));
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        QVERIFY2(!loadCert(QStringLiteral("ca")).isNull(), "test certificates missing; run gen-certs.sh");
        reportTlsEnvironment();
        traceMark(QStringLiteral("leave initTestCase"));
    }

    // Run before and after every test function. The last "enter" with no matching "leave" in the
    // trace names the clause the process died in -- the one fact the empty ctest log could not
    // give. A failed QVERIFY still reaches "leave" (QtTest runs cleanup after a failure), so an
    // "enter" with no "leave" is specifically a hard crash, not an assertion failure.
    void init()
    {
        traceMark(QStringLiteral("enter ") + QString::fromUtf8(QTest::currentTestFunction()));
    }

    void cleanup()
    {
        traceMark(QStringLiteral("leave ") + QString::fromUtf8(QTest::currentTestFunction()));
    }

    // Clauses 1 and 4: mutual TLS on loopback carries a property and a slot, each side
    // verified against the CA, and the owner sees the caller's certificate subject.
    void mutualTlsLoopbackExchange()
    {
        const QSslCertificate ca{loadCert(QStringLiteral("ca"))};

        MeshServer server;
        QVERIFY2(server.listenMutualTls(QHostAddress::LocalHost, 0, ca,
                                        loadCert(QStringLiteral("alpha")),
                                        loadKey(QStringLiteral("alpha"))),
                 qPrintable(server.errorString()));
        const quint16 port{server.serverPort()};

        QRemoteObjectHost host;
        host.setHostUrl(QUrl{QStringLiteral("synqt-m3-tls:///host")},
                        QRemoteObjectHost::AllowExternalRegistration);
        MeshBackend source;
        QVERIFY(host.enableRemoting<MeshSourceAPI>(&source));

        MeshPeer observedPeer;
        connect(&server, &MeshServer::peerConnected, &host,
                [&host, &observedPeer](QIODevice *device, const MeshPeer &peer) {
                    observedPeer = peer;
                    host.addHostSideConnection(device);
                });

        QRemoteObjectNode node;
        MeshClient client;
        connect(&client, &MeshClient::connected, &node,
                [&node](QIODevice *device) { node.addClientSideConnection(device); });
        // Both sides already work out why a handshake failed, and nothing was listening: a
        // failure arrived as a signal that never came and a timeout that named no cause.
        // Report them. On a green run these are silent; on a red one they are the difference
        // between "the peer's certificate was rejected, here is the error" and five seconds
        // of nothing.
        connect(&client, &MeshClient::errorOccurred, this, [](const QString &reason) {
            qWarning().noquote() << "mesh client error:" << reason;
        });
        connect(&server, &MeshServer::peerRejected, this, [](const QString &reason) {
            qWarning().noquote() << "mesh server rejected peer:" << reason;
        });
        QSignalSpy connectedSpy{&client, &MeshClient::connected};
        QVERIFY(client.connectMutualTls(QHostAddress::LocalHost, port,
                                        QStringLiteral("alpha"), ca,
                                        loadCert(QStringLiteral("beta")),
                                        loadKey(QStringLiteral("beta"))));
        QTRY_VERIFY_WITH_TIMEOUT(connectedSpy.count() >= 1, 5000);

        QScopedPointer<MeshReplica> replica{node.acquire<MeshReplica>()};
        QVERIFY(replica->waitForSource(5000));

        source.setValue(9);
        QTRY_COMPARE(replica->value(), 9);
        replica->poke(3);
        QTRY_COMPARE(source.lastPoke, 3);

        // The calling entity is the verified peer certificate's subject.
        QCOMPARE(observedPeer.entity, QStringLiteral("beta"));
        QVERIFY(observedPeer.authenticated);
    }

    // Clause 2: a consumer presenting no certificate is rejected at the handshake.
    void missingCertificateRejected()
    {
        const QSslCertificate ca{loadCert(QStringLiteral("ca"))};
        MeshServer server;
        QVERIFY(server.listenMutualTls(QHostAddress::LocalHost, 0, ca,
                                       loadCert(QStringLiteral("alpha")),
                                       loadKey(QStringLiteral("alpha"))));
        const quint16 port{server.serverPort()};

        QRemoteObjectHost host;
        host.setHostUrl(QUrl{QStringLiteral("synqt-m3-nocert:///host")},
                        QRemoteObjectHost::AllowExternalRegistration);
        MeshBackend source;
        host.enableRemoting<MeshSourceAPI>(&source);
        QSignalSpy peerSpy{&server, &MeshServer::peerConnected};
        connect(&server, &MeshServer::peerConnected, &host,
                [&host](QIODevice *device, const MeshPeer &) {
                    host.addHostSideConnection(device);
                });

        // Verifies the server against the CA, but presents no client certificate.
        QSslSocket bad;
        QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
        configuration.setCaCertificates({ca});
        configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
        bad.setSslConfiguration(configuration);
        QSignalSpy droppedSpy{&bad, &QSslSocket::disconnected};
        bad.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, QStringLiteral("alpha"));

        // The server rejects the peer at the handshake and drops it; no authenticated
        // peer ever reaches the host. (In TLS 1.3 the client may briefly compute an
        // encrypted channel before the server's rejection arrives, so the server side
        // is the authoritative check.)
        QTRY_VERIFY_WITH_TIMEOUT(droppedSpy.count() >= 1, 3000);
        QCOMPARE(peerSpy.count(), 0);
    }

    // Clause 3: a consumer presenting a certificate from a different CA is rejected.
    void foreignCertificateRejected()
    {
        const QSslCertificate ca{loadCert(QStringLiteral("ca"))};
        MeshServer server;
        QVERIFY(server.listenMutualTls(QHostAddress::LocalHost, 0, ca,
                                       loadCert(QStringLiteral("alpha")),
                                       loadKey(QStringLiteral("alpha"))));
        const quint16 port{server.serverPort()};

        QRemoteObjectHost host;
        host.setHostUrl(QUrl{QStringLiteral("synqt-m3-foreign:///host")},
                        QRemoteObjectHost::AllowExternalRegistration);
        MeshBackend source;
        host.enableRemoting<MeshSourceAPI>(&source);
        QSignalSpy peerSpy{&server, &MeshServer::peerConnected};
        connect(&server, &MeshServer::peerConnected, &host,
                [&host](QIODevice *device, const MeshPeer &) {
                    host.addHostSideConnection(device);
                });

        // Presents a certificate signed by an unrelated CA.
        QSslSocket bad;
        QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
        configuration.setCaCertificates({ca});
        configuration.setLocalCertificate(loadCert(QStringLiteral("rogue")));
        configuration.setPrivateKey(loadKey(QStringLiteral("rogue")));
        configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
        bad.setSslConfiguration(configuration);
        QSignalSpy droppedSpy{&bad, &QSslSocket::disconnected};
        bad.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, QStringLiteral("alpha"));

        QTRY_VERIFY_WITH_TIMEOUT(droppedSpy.count() >= 1, 3000);
        QCOMPARE(peerSpy.count(), 0);
    }

    // Clause 5: the explicitly opted-in local socket carries a property and a slot; its
    // peer is trusted by colocation, not authenticated.
    void localSocketExchange()
    {
        const QString socketName{
            QStringLiteral("synqt-m3-%1").arg(QCoreApplication::applicationPid())};

        traceMark(QStringLiteral("local: listenLocal ") + socketName);
        MeshServer server;
        QVERIFY2(server.listenLocal(socketName, QStringLiteral("beta")),
                 qPrintable(server.errorString()));

        // The socket is restricted to the run-as user (MeshServer sets
        // QLocalServer::UserAccessOption). How that restriction is expressed, and thus how it
        // can be checked, is platform-specific: on a POSIX host the endpoint is a filesystem
        // socket file carrying Unix mode bits, so assert no group/other access; on Windows it
        // is a named pipe (\\.\pipe\...) secured by an ACL in its security descriptor, not by
        // mode bits, and QFileInfo::permissions() fabricates read bits for it that mean
        // nothing, so the POSIX check does not apply there.
        traceMark(QStringLiteral("local: localSocketFullName"));
        const QString socketFile{server.localSocketFullName()};
        traceMark(QStringLiteral("local: fullName=") + socketFile);
        QVERIFY(!socketFile.isEmpty());
#ifndef Q_OS_WIN
        const QFileInfo socketInfo{socketFile};
        if (socketInfo.exists()) {
            traceMark(QStringLiteral("local: checking permissions"));
            const QFile::Permissions groupOther{
                QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                QFile::ReadOther | QFile::WriteOther | QFile::ExeOther};
            QVERIFY2(!(socketInfo.permissions() & groupOther),
                     "local socket file is not restricted to the run-as user");
        }
#endif

        traceMark(QStringLiteral("local: enableRemoting"));
        QRemoteObjectHost host;
        host.setHostUrl(QUrl{QStringLiteral("synqt-m3-local:///host")},
                        QRemoteObjectHost::AllowExternalRegistration);
        MeshBackend source;
        QVERIFY(host.enableRemoting<MeshSourceAPI>(&source));

        MeshPeer observedPeer;
        connect(&server, &MeshServer::peerConnected, &host,
                [&host, &observedPeer](QIODevice *device, const MeshPeer &peer) {
                    traceMark(QStringLiteral("local: peerConnected addHostSideConnection"));
                    observedPeer = peer;
                    host.addHostSideConnection(device);
                });

        QRemoteObjectNode node;
        MeshClient client;
        connect(&client, &MeshClient::connected, &node,
                [&node](QIODevice *device) {
                    traceMark(QStringLiteral("local: client connected addClientSideConnection"));
                    node.addClientSideConnection(device);
                });
        QSignalSpy connectedSpy{&client, &MeshClient::connected};
        traceMark(QStringLiteral("local: connectLocal"));
        QVERIFY(client.connectLocal(socketName));
        // A local socket connects synchronously, so check the count (QTRY handles both
        // an already-emitted and a still-pending connection).
        QTRY_VERIFY_WITH_TIMEOUT(connectedSpy.count() >= 1, 5000);

        traceMark(QStringLiteral("local: acquire replica"));
        QScopedPointer<MeshReplica> replica{node.acquire<MeshReplica>()};
        QVERIFY(replica->waitForSource(5000));

        traceMark(QStringLiteral("local: exchange prop+slot"));
        source.setValue(5);
        QTRY_COMPARE(replica->value(), 5);
        replica->poke(8);
        QTRY_COMPARE(source.lastPoke, 8);

        // Colocation trust, not authentication.
        traceMark(QStringLiteral("local: verify peer"));
        QCOMPARE(observedPeer.entity, QStringLiteral("beta"));
        QVERIFY(!observedPeer.authenticated);
    }
};

// The expansion of QTEST_GUILESS_MAIN, with one addition: stdout and stderr are made
// unbuffered before the run. On Windows a piped standard stream is block-buffered, so a
// hard termination (an access violation or an abort, the kind a failed QVERIFY never
// produces) discards everything QtTest had printed into the buffer and the failure reaches
// the log as zero output -- which is exactly how m3 first surfaced: "***Failed" in half a
// second with not one PASS or QWARN line to say where. Unbuffering flushes each line as it
// is written, so the last line before a crash is the crash site. It costs nothing on a green
// run and nothing on the other platforms, where a pipe is line-buffered already.
int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // The trace begins here: an empty or absent file after a crash means the process died before
    // main (loading or static initialization), which is a different bug from a crash in a test.
    traceReset(QStringLiteral("main: enter"));
    QCoreApplication app{argc, argv};
    QCoreApplication::setAttribute(Qt::AA_Use96Dpi, true);
    traceMark(QStringLiteral("main: app constructed"));
    TestM3 testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    traceMark(QStringLiteral("main: qExec begin"));
    const int result{QTest::qExec(&testObject, argc, argv)};
    traceMark(QStringLiteral("main: qExec returned ") + QString::number(result));
    return result;
}

#include "tst_m3.moc"
