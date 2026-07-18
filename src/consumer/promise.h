// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PROMISE_H
#define SYNQT_PROMISE_H

#include <QJSValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

#include <QtRemoteObjects/QRemoteObjectPendingCall>

QT_BEGIN_NAMESPACE
class QJSEngine;
QT_END_NAMESPACE

namespace SynQt {

/// A small JS-thenable over a QtRO pending reply, so a consumer facade's returning-slot
/// call reads in QML as `Server.x.slot(args).then(value => ...)`. `then(onFulfilled)` runs
/// its callback with the owner's reply value once it arrives (or immediately if already
/// settled); `catchError(onRejected)` runs its callback with a reason string if the call
/// failed or the connect point was not live. Both return a new Promise resolved with the
/// callback's return value, so `.then(...).catchError(...)` chains in the usual way.
class Promise : public QObject
{
    Q_OBJECT

public:
    /// `engine` is the QML/JS engine the QML callbacks belong to (the facade passes its
    /// qmlEngine); it converts the reply value to the callback argument. It may be null in a
    /// pure-C++ setting, where the value is passed through best-effort.
    Promise(const QRemoteObjectPendingCall &call, QJSEngine *engine, QObject *parent = nullptr);

    /// An already-settled promise, for the paths that resolve without a remote call (the
    /// connect point is not live, or a value is known synchronously).
    static Promise *resolved(const QVariant &value, QJSEngine *engine, QObject *parent = nullptr);
    static Promise *rejected(const QString &reason, QJSEngine *engine, QObject *parent = nullptr);

    Q_INVOKABLE SynQt::Promise *then(const QJSValue &onFulfilled);
    Q_INVOKABLE SynQt::Promise *catchError(const QJSValue &onRejected);

private:
    enum class State { Pending, Fulfilled, Rejected };

    struct Handler {
        QJSValue callback;
        Promise *next{nullptr};
        bool onRejected{false};
    };

    explicit Promise(QJSEngine *engine, QObject *parent = nullptr);

    void settleFulfilled(const QVariant &value);
    void settleRejected(const QString &reason);
    void settleFromCall(const QRemoteObjectPendingCall &call);
    void addHandler(const QJSValue &callback, Promise *next, bool onRejected);
    void flush();
    void dispatch(const Handler &handler);

    QJSEngine *m_engine{nullptr};
    State m_state{State::Pending};
    QVariant m_value;
    QString m_reason;
    QList<Handler> m_handlers;
};

} // namespace SynQt

#endif // SYNQT_PROMISE_H
