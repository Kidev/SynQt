// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_M0_M0CONTROLLER_H
#define SYNQT_M0_M0CONTROLLER_H

#include <QAbstractItemModel>
#include <QObject>
#include <QRemoteObjectPendingCall>
#include <QString>
#include <QUrl>

QT_BEGIN_NAMESPACE
class QRemoteObjectNode;
class QTimer;
class QWebSocket;
QT_END_NAMESPACE

class SpikeSourceReplica;
class WebSocketIoDevice;

// Owns the QtRO transport for the client and exposes the live results of every
// direction to QML. All transport wiring is done here in C++ (the QtRO QML Node
// type cannot take an externally connected transport). Progress is also emitted to
// the browser console as single-line "M0 ..." sentinels that the verify harness
// asserts on.
class M0Controller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(int counter READ counter NOTIFY counterChanged)
    Q_PROPERTY(QString lastSignal READ lastSignal NOTIFY lastSignalChanged)
    Q_PROPERTY(QString lastReply READ lastReply NOTIFY lastReplyChanged)
    Q_PROPERTY(int modelRows READ modelRows NOTIFY modelRowsChanged)
    Q_PROPERTY(QAbstractItemModel *rowsModel READ rowsModel NOTIFY rowsModelChanged)

public:
    explicit M0Controller(const QUrl &edgeUrl, QObject *parent = nullptr);
    ~M0Controller() override;

    QString state() const;
    int counter() const;
    QString lastSignal() const;
    QString lastReply() const;
    int modelRows() const;
    QAbstractItemModel *rowsModel() const;

    Q_INVOKABLE void callEcho(const QString &message);

signals:
    void stateChanged();
    void counterChanged();
    void lastSignalChanged();
    void lastReplyChanged();
    void modelRowsChanged();
    void rowsModelChanged();

private:
    void connectToEdge();
    void teardown();
    void scheduleReconnect();
    void onReplicaInitialized();
    void refreshModelRows();
    void setState(const QString &state);

    const QUrl m_url;
    QRemoteObjectNode *m_node{nullptr};
    QWebSocket *m_socket{nullptr};
    WebSocketIoDevice *m_device{nullptr};
    SpikeSourceReplica *m_replica{nullptr};
    QAbstractItemModel *m_rowsModel{nullptr};
    QTimer *m_reconnectTimer{nullptr};
    QTimer *m_echoRetryTimer{nullptr};
    // firefox-on-WASM reply-path fallback (CONFIRMED in CI): the latest echo reply, held so a
    // 250ms timer can resolve it from its own isFinished()/returnValue() state when QtRO's queued
    // watcher finished() signal is starved by the WASM posted-event pump. See the constructor.
    QTimer *m_replyPollTimer{nullptr};
    QRemoteObjectPendingReply<QString> m_pendingReply;
    QString m_state{QStringLiteral("idle")};
    int m_counter{0};
    QString m_lastSignal;
    QString m_lastReply;
    int m_modelRows{0};
    int m_backoffMs{500};
};

#endif // SYNQT_M0_M0CONTROLLER_H
