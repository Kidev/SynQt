// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "promise.h"

#include "deletesoon.h"
#include "tracescope.h"

#include <QJSEngine>
#include <QPointer>
#include <QTimer>

#include <QtRemoteObjects/QRemoteObjectPendingCallWatcher>

namespace SynQt {

/// Settles one Promise from JavaScript: what a thenable's own then() calls back.
class PromiseSettler : public QObject
{
    Q_OBJECT

public:
    explicit PromiseSettler(Promise *target)
        : m_target{target}
    {
    }

    Q_INVOKABLE void fulfil(const QJSValue &value)
    {
        if (m_target) {
            m_target->resolve(value.toVariant());
        }
    }

    Q_INVOKABLE void reject(const QJSValue &reason)
    {
        if (m_target) {
            m_target->reject(reason.toString());
        }
    }

private:
    QPointer<Promise> m_target;
};

Promise::Promise(QJSEngine *engine, QObject *parent)
    : QObject{parent}
    , m_engine{engine}
    , m_trace{TraceScope::current()}
{
}

Promise::Promise(const QRemoteObjectPendingCall &call, QJSEngine *engine, QObject *parent)
    : QObject{parent}
    , m_engine{engine}
    , m_trace{TraceScope::current()}
{
    if (call.isFinished()) {
        settleFromCall(call);
        return;
    }
    QRemoteObjectPendingCallWatcher *watcher{new QRemoteObjectPendingCallWatcher{call, this}};
    connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
            [this](QRemoteObjectPendingCallWatcher *finished) {
                settleFromCall(*finished);
                deleteSoon(finished);
            });

#ifdef Q_OS_WASM
    // On Firefox for WebAssembly the posted-event pump that delivers the watcher's queued
    // finished() can be starved: the reply resolves (isFinished() is true, its value set
    // synchronously in QtRO's notifyAboutReply) but the watcher never fires. Poll the
    // call's own state as a fallback. settleFulfilled/settleRejected are guarded on
    // Pending, so the first path wins. WASM only: native builds drain posted events
    // normally.
    QTimer *poll{new QTimer{this}};
    poll->setInterval(50);
    connect(poll, &QTimer::timeout, this, [this, call, poll]() {
        if (m_state != State::Pending) {
            poll->stop();
            return;
        }
        if (call.isFinished()) {
            poll->stop();
            settleFromCall(call);
        }
    });
    poll->start();
#endif
}

void Promise::settleFromCall(const QRemoteObjectPendingCall &call)
{
    if (call.error() == QRemoteObjectPendingCall::NoError) {
        settleFulfilled(call.returnValue());
    } else {
        settleRejected(QStringLiteral("the remote call failed"));
    }
}

Promise *Promise::resolved(const QVariant &value, QJSEngine *engine, QObject *parent)
{
    Promise *promise{new Promise{engine, parent}};
    promise->settleFulfilled(value);
    return promise;
}

Promise *Promise::rejected(const QString &reason, QJSEngine *engine, QObject *parent)
{
    Promise *promise{new Promise{engine, parent}};
    promise->settleRejected(reason);
    return promise;
}

Promise *Promise::pending(QJSEngine *engine, QObject *parent)
{
    return new Promise{engine, parent};
}

void Promise::resolve(const QVariant &value)
{
    settleFulfilled(value);
}

void Promise::reject(const QString &reason)
{
    settleRejected(reason);
}

SynQt::Promise *Promise::then(const QJSValue &onFulfilled, const QJSValue &onRejected)
{
    Promise *next{new Promise{m_engine, this}};
    addHandler(onFulfilled, onRejected, next);
    return next;
}

SynQt::Promise *Promise::catchError(const QJSValue &onRejected)
{
    Promise *next{new Promise{m_engine, this}};
    addHandler(QJSValue{}, onRejected, next);
    return next;
}

void Promise::addHandler(const QJSValue &onFulfilled, const QJSValue &onRejected,
                         Promise *next)
{
    m_handlers.append(Handler{onFulfilled, onRejected, next});
    if (m_state != State::Pending) {
        flush();
    }
}

void Promise::settleFulfilled(const QVariant &value)
{
    if (m_state != State::Pending) {
        return;
    }
    m_state = State::Fulfilled;
    m_value = value;
    flush();
}

void Promise::abandon(const QString &reason)
{
    settleRejected(reason);
}

void Promise::settleRejected(const QString &reason)
{
    if (m_state != State::Pending) {
        return;
    }
    m_state = State::Rejected;
    m_reason = reason;
    flush();
}

void Promise::flush()
{
    const QList<Handler> handlers{m_handlers};
    m_handlers.clear();
    for (const Handler &handler : handlers) {
        dispatch(handler);
    }
    scheduleDisposal();
}

void Promise::scheduleDisposal()
{
    // After the current turn, so everything chained here runs first and a running handler
    // is not freed under itself. A chained promise is a child of the one it was chained
    // from, so retiring the root retires the chain. A child that scheduled its own disposal
    // first is deleted by its parent, and Qt drops the pending deletion with it.
    if (m_disposalScheduled) {
        return;
    }
    m_disposalScheduled = true;
    deleteSoon(this);
}

void Promise::dispatch(const Handler &handler)
{
    // A fulfilled value flows through onFulfilled, a rejection through onRejected (which
    // recovers the chain). With no handler for this outcome, it passes on unchanged: a
    // rejection through `catchError(undefined)` stays a rejection, instead of becoming a
    // fulfilment with an empty value.
    QJSValue callback{m_state == State::Fulfilled ? handler.onFulfilled : handler.onRejected};
    if (!callback.isCallable()) {
        if (m_state == State::Fulfilled) {
            handler.next->settleFulfilled(m_value);
        } else {
            handler.next->settleRejected(m_reason);
        }
        return;
    }
    QJSValue argument{m_engine != nullptr
                          ? (m_state == State::Fulfilled ? m_engine->toScriptValue(m_value)
                                                         : m_engine->toScriptValue(m_reason))
                          : QJSValue{}};
    // The handler runs in the calling context for tracing: its outbound calls and records
    // join the click's trace.
    const TraceScope scope{m_trace};
    const QJSValue result{callback.call(QJSValueList{argument})};
    if (result.isError()) {
        handler.next->settleRejected(result.toString());
        return;
    }
    if (result.isObject() && result.property(QStringLiteral("then")).isCallable()) {
        handler.next->follow(result);
        return;
    }
    handler.next->settleFulfilled(result.toVariant());
}

void Promise::follow(const QJSValue &thenable)
{
    // This promise is a child of the one whose handler returned `thenable`, and that one is
    // retired a turn after it settled. Moved under whatever settles it now, so it outlives
    // the wait.
    if (Promise *source{qobject_cast<Promise *>(thenable.toQObject())}) {
        if (source != this) {
            setParent(source);
            source->addHandler(QJSValue{}, QJSValue{}, this);
        }
        return;
    }
    if (m_engine == nullptr) {
        settleRejected(QStringLiteral("a handler returned a promise with no engine to wait on"));
        return;
    }
    // Any other thenable (a JavaScript Promise): its own then() settles this one. The
    // settler is owned by the engine and kept alive by the callbacks `thenable` holds.
    PromiseSettler *settler{new PromiseSettler{this}};
    setParent(settler);
    const QJSValue handle{m_engine->newQObject(settler)};
    QJSValue then{thenable.property(QStringLiteral("then"))};
    then.callWithInstance(thenable, QJSValueList{handle.property(QStringLiteral("fulfil")),
                                                  handle.property(QStringLiteral("reject"))});
}

} // namespace SynQt

#include "promise.moc"
