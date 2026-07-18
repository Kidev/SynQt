// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "m0controller.h"

#include "rep_spike_replica.h"
#include "websocketiodevice.h"

#include <QAbstractItemModel>
#include <QAbstractItemModelReplica>
#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCall>
#include <QTimer>
#include <QWebSocket>

#include <algorithm>

namespace {
const QString kStateConnecting{QStringLiteral("connecting")};
const QString kStateConnected{QStringLiteral("connected")};
const QString kStateDisconnected{QStringLiteral("disconnected")};
} // namespace

M0Controller::M0Controller(const QUrl &edgeUrl, QObject *parent)
    : QObject{parent}
    , m_url{edgeUrl}
{
    m_reconnectTimer = new QTimer{this};
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() { connectToEdge(); });

    // The slot round-trip is proven by a single echo call, and a single unacknowledged
    // request with no retry is fragile: if that one reply frame is lost or arrives before
    // the watcher is wired, the fourth direction reads as broken for the whole run. This
    // timer re-issues the echo until a reply resolves (it stops itself on the first one),
    // so the check tolerates a dropped request the way any real RPC client must. On every
    // engine that answers the first call this fires zero extra times; where the first reply
    // never comes back it keeps asking, which is also the honest signal that the loss is
    // persistent rather than a one-shot race.
    m_echoRetryTimer = new QTimer{this};
    m_echoRetryTimer->setInterval(2000);
    connect(m_echoRetryTimer, &QTimer::timeout, this, [this]() {
        if (m_lastReply.isEmpty()) {
            callEcho(QStringLiteral("m0-ping"));
        }
    });

    // Poll-fallback for the firefox-on-WASM reply path (CONFIRMED in CI): notifyAboutReply
    // (replica.cpp) sets error=NoError and returnValue SYNCHRONOUSLY from the socket read callback,
    // so isFinished() flips the instant the reply frame is processed -- independent of the watcher's
    // finished() signal, which QtRO fires over a Qt::QueuedConnection (qremoteobjectpendingcall.cpp
    // 32-34) that needs the posted-event pump to drain. On firefox-WASM in CI that pump is starved,
    // so the watcher never fires though the reply data is present and correct. This timer resolves
    // the reply from its own state when the watcher has not, so a starved queued signal cannot
    // strand the fourth direction. It is logged "(via poll fallback)" so the workaround is always
    // visible, never silent; on every engine where the watcher fires it is a no-op.
    m_replyPollTimer = new QTimer{this};
    m_replyPollTimer->setInterval(250);
    connect(m_replyPollTimer, &QTimer::timeout, this, [this]() {
        // A default-constructed reply reports error==InvalidMessage, so isFinished() stays false
        // until a real reply resolves it; no separate "has a call been issued" guard is needed.
        if (!m_lastReply.isEmpty() || !m_pendingReply.isFinished()
            || m_pendingReply.error() != QRemoteObjectPendingCall::NoError) {
            return;
        }
        m_lastReply = m_pendingReply.returnValue();
        emit lastReplyChanged();
        m_echoRetryTimer->stop();
        m_replyPollTimer->stop();
        qInfo().noquote()
            << QStringLiteral("M0 slot reply=%1 (via poll fallback)").arg(m_lastReply);
    });

    connectToEdge();
}

M0Controller::~M0Controller()
{
    teardown();
}

QString M0Controller::state() const
{
    return m_state;
}

int M0Controller::counter() const
{
    return m_counter;
}

QString M0Controller::lastSignal() const
{
    return m_lastSignal;
}

QString M0Controller::lastReply() const
{
    return m_lastReply;
}

int M0Controller::modelRows() const
{
    return m_modelRows;
}

QAbstractItemModel *M0Controller::rowsModel() const
{
    return m_rowsModel;
}

void M0Controller::setState(const QString &state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

void M0Controller::connectToEdge()
{
    teardown();
    setState(kStateConnecting);
    qInfo().noquote()
        << QStringLiteral("M0 state=connecting url=%1").arg(m_url.toString());

    // Build a fresh node + socket + adapter on every attempt so a reconnect is a
    // clean re-handshake rather than a reuse of torn-down transport state.
    m_node = new QRemoteObjectNode{this};
    m_socket = new QWebSocket{};
    m_device = new WebSocketIoDevice{m_socket, this};

    // Add the client-side connection and set the heartbeat BEFORE opening the
    // socket: the QtRO handshake is server-initiated, so the client only writes
    // after the socket is connected and the server's init packet arrives.
    m_node->addClientSideConnection(m_device);
    m_node->setHeartbeatInterval(1000);

    m_replica = m_node->acquire<SpikeSourceReplica>();
    connect(m_replica, &SpikeSourceReplica::initialized,
            this, &M0Controller::onReplicaInitialized);
    connect(m_replica, &SpikeSourceReplica::counterChanged, this, [this](int value) {
        m_counter = value;
        emit counterChanged();
        qInfo().noquote() << QStringLiteral("M0 prop counter=%1").arg(value);
    });
    connect(m_replica, &SpikeSourceReplica::pinged, this, [this](const QString &payload) {
        m_lastSignal = payload;
        emit lastSignalChanged();
        qInfo().noquote() << QStringLiteral("M0 signal payload=%1").arg(payload);
    });

    connect(m_socket, &QWebSocket::disconnected, this, [this]() {
        qInfo().noquote() << QStringLiteral("M0 socket disconnected");
        scheduleReconnect();
    });
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        qInfo().noquote()
            << QStringLiteral("M0 socket error=%1").arg(m_socket->errorString());
        scheduleReconnect();
    });

    m_socket->open(m_url);
}

void M0Controller::onReplicaInitialized()
{
    setState(kStateConnected);
    m_backoffMs = 500;
    qInfo().noquote() << QStringLiteral("M0 state=connected");

    m_rowsModel = m_replica->rows();
    emit rowsModelChanged();
    if (m_rowsModel) {
        auto update = [this]() { refreshModelRows(); };
        if (auto *modelReplica = qobject_cast<QAbstractItemModelReplica *>(m_rowsModel)) {
            connect(modelReplica, &QAbstractItemModelReplica::initialized, this, update);
        }
        connect(m_rowsModel, &QAbstractItemModel::rowsInserted, this, update);
        connect(m_rowsModel, &QAbstractItemModel::modelReset, this, update);
        connect(m_rowsModel, &QAbstractItemModel::dataChanged, this, update);
    }
    refreshModelRows();

    // Drive the slot round-trip (Replica -> Source) and resolve its return value
    // on the client, proving the fourth direction without any user interaction. The
    // retry timer re-issues it until a reply lands (see the constructor); it stops
    // itself once m_lastReply is set.
    callEcho(QStringLiteral("m0-ping"));
    m_echoRetryTimer->start();
}

void M0Controller::refreshModelRows()
{
    const int rows{m_rowsModel ? m_rowsModel->rowCount() : 0};
    if (rows != m_modelRows) {
        m_modelRows = rows;
        emit modelRowsChanged();
    }
    qInfo().noquote() << QStringLiteral("M0 model rows=%1").arg(m_modelRows);
}

void M0Controller::callEcho(const QString &message)
{
    if (!m_replica || !m_replica->isReplicaValid()) {
        return;
    }
    // Mark the moment the invoke leaves the client, so the frame-size instrument's rx lines
    // that follow can be read against the edge's "M0 EDGE echo invoked" + reply tx frame: this
    // is the anchor that turns the per-frame sizes into a reply-arrived-or-not verdict.
    qInfo().noquote() << QStringLiteral("M0 echo sent message=%1").arg(message);
    QRemoteObjectPendingReply<QString> reply{m_replica->echo(message)};
    // Hand the same reply to the passive poll (constructor) so it can observe the reply's own
    // resolved state independently of the queued watcher signal below. Diagnostic only.
    m_pendingReply = reply;
    if (m_lastReply.isEmpty() && !m_replyPollTimer->isActive()) {
        m_replyPollTimer->start();
    }
    QRemoteObjectPendingCallWatcher *watcher{
        new QRemoteObjectPendingCallWatcher{reply, this}};
    connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
            [this, reply, watcher]() mutable {
                if (reply.error() == QRemoteObjectPendingCall::NoError) {
                    m_lastReply = reply.returnValue();
                    emit lastReplyChanged();
                    m_echoRetryTimer->stop();
                    qInfo().noquote()
                        << QStringLiteral("M0 slot reply=%1").arg(m_lastReply);
                } else {
                    qInfo().noquote() << QStringLiteral("M0 slot error");
                }
                watcher->deleteLater();
            });
}

void M0Controller::scheduleReconnect()
{
    if (m_reconnectTimer->isActive()) {
        return;
    }
    if (m_state != kStateDisconnected) {
        setState(kStateDisconnected);
        qInfo().noquote() << QStringLiteral("M0 state=disconnected");
    }
    m_reconnectTimer->start(m_backoffMs);
    m_backoffMs = std::min(m_backoffMs * 2, 5000);
}

void M0Controller::teardown()
{
    if (m_echoRetryTimer) {
        m_echoRetryTimer->stop();
    }
    if (m_replyPollTimer) {
        m_replyPollTimer->stop();
    }
    // Drop the shared reply data (its d references the replica impl about to be torn down) so a
    // reconnect re-observes a fresh reply.
    m_pendingReply = QRemoteObjectPendingReply<QString>{};
    m_replica = nullptr;   // owned by m_node, deleted with it
    m_rowsModel = nullptr; // owned by the replica
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_device) {
        m_device->deleteLater();
        m_device = nullptr;
    }
    if (m_node) {
        m_node->deleteLater();
        m_node = nullptr;
    }
}
