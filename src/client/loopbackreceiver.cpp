// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "loopbackreceiver.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace SynQt {

namespace {

/// How much of a request this reads before giving up. A browser redirect is a GET with a
/// few headers; anything larger is broken or a local process trying to make the app buffer.
constexpr qint64 kMaxRequestBytes{8 * 1024};

/// How many sockets may be open at once. A browser opens one or two (the redirect and a
/// favicon); the cap stops a local process from parking connections here.
constexpr int kMaxConnections{8};

constexpr int kDefaultTimeoutMs{5 * 60 * 1000};

/// The page the visitor sees: fixed text with nothing from the request, so nothing can be
/// reflected.
const char *const kDonePage{
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<title>Signed in</title></head><body style=\"font:16px system-ui;padding:3em\">"
    "<p>Signed in. You can close this window and go back to the app.</p>"
    "</body></html>"};

} // namespace

LoopbackReceiver::LoopbackReceiver(QObject *parent)
    : QObject{parent}
    , m_server{new QTcpServer{this}}
    , m_deadline{new QTimer{this}}
{
    m_deadline->setSingleShot(true);
    m_deadline->setInterval(kDefaultTimeoutMs);
    connect(m_deadline, &QTimer::timeout, this, [this]() {
        if (!m_finished) {
            m_finished = true;
            stop();
            emit timedOut();
        }
    });
    connect(m_server, &QTcpServer::newConnection, this, &LoopbackReceiver::onNewConnection);
}

LoopbackReceiver::~LoopbackReceiver() = default;

bool LoopbackReceiver::listen()
{
    // QHostAddress::LocalHost, not Any: reachable only from this machine. Port 0 lets the
    // OS pick a free port, so two apps signing in at once do not collide.
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        return false;
    }
    m_deadline->start();
    return true;
}

QString LoopbackReceiver::returnUrl() const
{
    if (!m_server->isListening()) {
        return QString{};
    }
    return QStringLiteral("http://127.0.0.1:%1/").arg(m_server->serverPort());
}

quint16 LoopbackReceiver::port() const
{
    return m_server->serverPort();
}

void LoopbackReceiver::setTimeout(int milliseconds)
{
    m_deadline->setInterval(milliseconds);
    if (m_deadline->isActive()) {
        m_deadline->start();
    }
}

void LoopbackReceiver::stop()
{
    m_deadline->stop();
    m_server->close();
    for (auto it{m_buffers.cbegin()}; it != m_buffers.cend(); ++it) {
        QTcpSocket *socket{it.key()};
        socket->disconnect(this);
        socket->close();
        socket->deleteLater();
    }
    m_buffers.clear();
}

void LoopbackReceiver::onNewConnection()
{
    while (QTcpSocket *socket{m_server->nextPendingConnection()}) {
        // Bound to loopback, so this cannot fail; checked anyway, since it decides whether
        // a stranger could hand this app a session.
        if (!socket->peerAddress().isLoopback() || m_buffers.size() >= kMaxConnections) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_buffers.insert(socket, QByteArray{});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            onReadyRead(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void LoopbackReceiver::onReadyRead(QTcpSocket *socket)
{
    auto buffer{m_buffers.find(socket)};
    if (buffer == m_buffers.end()) {
        return;
    }
    buffer->append(socket->readAll());
    if (buffer->size() > kMaxRequestBytes) {
        socket->abort();
        return;
    }
    // Only the request line matters, ending at the first newline; waiting for the blank
    // line after the headers would wait on a client that may never send it.
    const qsizetype lineEnd{buffer->indexOf('\n')};
    if (lineEnd < 0) {
        return;
    }
    const QByteArray line{buffer->left(lineEnd).trimmed()};
    const QList<QByteArray> parts{line.split(' ')};
    if (parts.size() < 2 || parts.at(0) != QByteArrayLiteral("GET")) {
        respond(socket, "405 Method Not Allowed", QByteArrayLiteral("no"));
        return;
    }

    const QUrl target{QString::fromUtf8(parts.at(1)), QUrl::StrictMode};
    const QUrlQuery query{target.query()};
    const QString code{query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded)};
    const QString error{query.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded)};
    if (code.isEmpty() && error.isEmpty()) {
        // A browser also asks for /favicon.ico; a stray request does not stop the wait.
        respond(socket, "404 Not Found", QByteArrayLiteral("no"));
        return;
    }

    const QString state{query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded)};
    respond(socket, "200 OK", QByteArray{kDonePage});
    finish(code, state, error);
}

void LoopbackReceiver::respond(QTcpSocket *socket, const char *status, const QByteArray &body)
{
    QByteArray response{"HTTP/1.1 "};
    response += status;
    response += "\r\nContent-Type: text/html; charset=utf-8"
                "\r\nCache-Control: no-store"
                "\r\nConnection: close"
                "\r\nContent-Length: ";
    response += QByteArray::number(body.size());
    response += "\r\n\r\n";
    response += body;
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

void LoopbackReceiver::finish(const QString &code, const QString &state, const QString &error)
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_deadline->stop();
    // Stop listening before reporting, so the caller's next steps run with the port closed.
    m_server->close();
    emit received(code, state, error);
}

} // namespace SynQt
