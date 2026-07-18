// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "spikesource.h"
#include "websocketiodevice.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QRemoteObjectHost>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

namespace {

// Wrap every accepted socket in the QIODevice adapter and hand it to the QtRO host.
// No registry: connections are added explicitly, per the SynQt deny-by-default rule.
void wireServer(QWebSocketServer *server, QRemoteObjectHost *host)
{
    QObject::connect(server, &QWebSocketServer::newConnection, host, [server, host]() {
        while (QWebSocket *connection{server->nextPendingConnection()}) {
            QObject::connect(connection, &QWebSocket::disconnected,
                             connection, &QWebSocket::deleteLater);
            QObject::connect(connection, &QWebSocket::errorOccurred,
                             connection, &QWebSocket::deleteLater);
            WebSocketIoDevice *ioDevice{new WebSocketIoDevice{connection}};
            QObject::connect(connection, &QObject::destroyed,
                             ioDevice, &WebSocketIoDevice::deleteLater);
            host->addHostSideConnection(ioDevice);
        }
    });
}

} // namespace

int main(int argc, char *argv[])
{
    QLoggingCategory::setFilterRules(QStringLiteral(
        "qt.remoteobjects.debug=false\nqt.remoteobjects.warning=false"));
    QCoreApplication app{argc, argv};
    QCoreApplication::setApplicationName(QStringLiteral("m0-edge"));

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption wsPortOption{QStringLiteral("ws-port"),
        QStringLiteral("Plaintext ws port."), QStringLiteral("port"),
        QStringLiteral("8088")};
    QCommandLineOption wssPortOption{QStringLiteral("wss-port"),
        QStringLiteral("Secure wss port."), QStringLiteral("port"),
        QStringLiteral("8089")};
    QCommandLineOption certOption{QStringLiteral("cert"),
        QStringLiteral("TLS certificate (PEM) enabling the wss listener."),
        QStringLiteral("file")};
    QCommandLineOption keyOption{QStringLiteral("key"),
        QStringLiteral("TLS private key (PEM) for the wss listener."),
        QStringLiteral("file")};
    parser.addOptions({wsPortOption, wssPortOption, certOption, keyOption});
    parser.process(app);

    const quint16 wsPort{parser.value(wsPortOption).toUShort()};
    const quint16 wssPort{parser.value(wssPortOption).toUShort()};

    SpikeSource source;

    // A unique, external-schema host URL: AllowExternalRegistration accepts it
    // without binding a native QtRO server; the real transports are the WebSocket
    // connections added below.
    QRemoteObjectHost host;
    host.setHostUrl(QUrl{QStringLiteral("synqt-m0:///edge")},
                    QRemoteObjectHost::AllowExternalRegistration);
    host.enableRemoting<SpikeSourceSourceAPI>(&source);

    // Plaintext ws listener: always on.
    QWebSocketServer *wsServer{new QWebSocketServer{
        QStringLiteral("m0-edge-ws"), QWebSocketServer::NonSecureMode, &app}};
    if (!wsServer->listen(QHostAddress::LocalHost, wsPort)) {
        qCritical().noquote()
            << QStringLiteral("M0 edge: ws listen failed on port %1").arg(wsPort);
        return 1;
    }
    wireServer(wsServer, &host);

    // Secure wss listener: only when a cert and key are supplied. The browser
    // presents no client certificate, so only the server is authenticated.
    bool wssUp{false};
    if (parser.isSet(certOption) && parser.isSet(keyOption)) {
        QFile certFile{parser.value(certOption)};
        QFile keyFile{parser.value(keyOption)};
        if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
            qCritical().noquote()
                << QStringLiteral("M0 edge: cannot open cert or key file");
            return 1;
        }
        QSslConfiguration tlsConfig{QSslConfiguration::defaultConfiguration()};
        tlsConfig.setLocalCertificate(QSslCertificate{certFile.readAll(), QSsl::Pem});
        tlsConfig.setPrivateKey(QSslKey{keyFile.readAll(), QSsl::Rsa, QSsl::Pem});
        tlsConfig.setPeerVerifyMode(QSslSocket::VerifyNone);

        QWebSocketServer *wssServer{new QWebSocketServer{
            QStringLiteral("m0-edge-wss"), QWebSocketServer::SecureMode, &app}};
        wssServer->setSslConfiguration(tlsConfig);
        if (!wssServer->listen(QHostAddress::LocalHost, wssPort)) {
            qCritical().noquote()
                << QStringLiteral("M0 edge: wss listen failed on port %1").arg(wssPort);
            return 1;
        }
        wireServer(wssServer, &host);
        wssUp = true;
    }

    qInfo().noquote() << QStringLiteral("M0 edge listening ws=%1 wss=%2")
                             .arg(QString::number(wsPort),
                                  wssUp ? QString::number(wssPort)
                                        : QStringLiteral("off"));
    return app.exec();
}
