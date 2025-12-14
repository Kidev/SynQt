// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_LOOPBACKRECEIVER_H
#define SYNQT_LOOPBACKRECEIVER_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QTcpServer;
class QTcpSocket;
class QTimer;
QT_END_NAMESPACE

namespace SynQt {

/// The desktop client's half of the loopback redirect. A listener that exists only for the
/// length of one sign-in, receives the edge's answer out of the system browser, and stops.
///
/// This is the native-app pattern of RFC 8252. A desktop app has no origin and no cookie
/// jar, so the finished login has to come back over something the app controls, and the one
/// channel a browser can be redirected to on the same machine is a loopback port. What
/// arrives here is a claim code, never the session itself, because the URL a browser is
/// sent to is written into that browser's history. See
/// [Desktop](https://synqt.org/desktop/).
///
/// Bound to 127.0.0.1 and nothing else. Binding the wildcard would offer the claim code to
/// the network, which is the difference between a local hop and a broadcast.
class LoopbackReceiver : public QObject
{
    Q_OBJECT

public:
    explicit LoopbackReceiver(QObject *parent = nullptr);
    ~LoopbackReceiver() override;

    /// Take an ephemeral port on the loopback interface. False when the OS refuses, which
    /// is a reason to abandon the sign-in rather than open a browser at a URL whose answer
    /// has nowhere to land.
    bool listen();

    /// The address to hand the edge as `return`, "http://127.0.0.1:<port>/". Empty until
    /// listen() has succeeded.
    QString returnUrl() const;

    quint16 port() const;

    /// Stop listening and drop every connection. Called by the receiver itself as soon as
    /// the answer arrives, so the window in which anything can reach this port is the
    /// length of one sign-in and not the life of the app.
    void stop();

    /// How long to wait for the visitor to finish in the browser. Five minutes by default,
    /// which is the edge's own patience with a pending login, because a sign-in with a
    /// password manager and a second factor in it is not a one-minute thing.
    void setTimeout(int milliseconds);

signals:
    /// The browser arrived. Exactly one of `code` and `error` is set; `state` is whatever
    /// the redirect carried and is the caller's to check.
    void received(const QString &code, const QString &state, const QString &error);

    /// Nobody arrived before the deadline. The listener is already closed.
    void timedOut();

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    /// Answer this socket and close it. `body` is fixed text. Nothing a request carried is
    /// ever written into it.
    void respond(QTcpSocket *socket, const char *status, const QByteArray &body);
    void finish(const QString &code, const QString &state, const QString &error);

    QTcpServer *m_server;
    QTimer *m_deadline;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    bool m_finished{false};
};

} // namespace SynQt

#endif // SYNQT_LOOPBACKRECEIVER_H
