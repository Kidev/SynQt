// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PROMISE_H
#define SYNQT_PROMISE_H

#include "tracecontext.h"

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
/// settled). `catchError(onRejected)` runs its callback with a reason string if the call
/// failed or the connect point was not live. Both return a new Promise resolved with the
/// callback's return value, so `.then(...).catchError(...)` chains in the usual way.
///
/// A promise is a child of the facade and is disposed one event-loop turn after it settles,
/// after every handler chained onto it in the same turn has run. Attach handlers where the
/// call is made, not later to a promise kept in a property.
///
/// A handler runs in the trace of the call that made the promise: the promise keeps the
/// context current at its creation and restores it around each handler, so
/// `Db.read().then(rows => Cache.put(rows))` reaches the second entity as the same click. The
/// session is not kept, because a continuation acts for nobody (see ActingFor), and a trace
/// authorizes nothing.
class Promise : public QObject
{
    Q_OBJECT

public:
    /// `engine` is the QML/JS engine the QML callbacks belong to (the facade passes its
    /// qmlEngine). It converts the reply value to the callback argument. It may be null in a
    /// pure-C++ setting, where the value is passed through best-effort.
    Promise(const QRemoteObjectPendingCall &call, QJSEngine *engine, QObject *parent = nullptr);

    /// An already-settled promise, for the paths that resolve without a remote call (the
    /// connect point is not live, or a value is known synchronously).
    static Promise *resolved(const QVariant &value, QJSEngine *engine, QObject *parent = nullptr);
    static Promise *rejected(const QString &reason, QJSEngine *engine, QObject *parent = nullptr);

    Q_INVOKABLE SynQt::Promise *then(const QJSValue &onFulfilled);
    Q_INVOKABLE SynQt::Promise *catchError(const QJSValue &onRejected);

    /// Settle a promise whose answer is now known never to arrive, with `reason`.
    ///
    /// A remote call is answered on the connection it was sent on. The facade calls this for
    /// every promise it still holds when its Replica is replaced on a reconnect. A promise already
    /// settled is left alone.
    void abandon(const QString &reason);

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
    /// Retire this promise (and the chain parented to it) after the current turn.
    void scheduleDisposal();

    QJSEngine *m_engine{nullptr};
    TraceContext m_trace;
    State m_state{State::Pending};
    QVariant m_value;
    QString m_reason;
    QList<Handler> m_handlers;
    bool m_disposalScheduled{false};
};

} // namespace SynQt

#endif // SYNQT_PROMISE_H
