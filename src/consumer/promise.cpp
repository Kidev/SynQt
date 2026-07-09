// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "promise.h"

#include "deletesoon.h"
#include "tracescope.h"

#include <QJSEngine>
#include <QTimer>

#include <QtRemoteObjects/QRemoteObjectPendingCallWatcher>

namespace SynQt {

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

SynQt::Promise *Promise::then(const QJSValue &onFulfilled)
{
    Promise *next{new Promise{m_engine, this}};
    addHandler(onFulfilled, next, false);
    return next;
}

SynQt::Promise *Promise::catchError(const QJSValue &onRejected)
{
    Promise *next{new Promise{m_engine, this}};
    addHandler(onRejected, next, true);
    return next;
}

void Promise::addHandler(const QJSValue &callback, Promise *next, bool onRejected)
{
    m_handlers.append(Handler{callback, next, onRejected});
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
    // A fulfilled value flows through onFulfilled. A rejection flows through onRejected
    // (which recovers the chain) or otherwise propagates unchanged.
    const bool runsHere{(m_state == State::Fulfilled && !handler.onRejected)
                        || (m_state == State::Rejected && handler.onRejected)};
    if (!runsHere) {
        if (m_state == State::Fulfilled) {
            handler.next->settleFulfilled(m_value);
        } else {
            handler.next->settleRejected(m_reason);
        }
        return;
    }

    QJSValue callback{handler.callback};
    if (!callback.isCallable()) {
        // No handler, so the outcome passes through: a rejection through
        // `catchError(undefined)` stays a rejection, instead of becoming a fulfilment with
        // an empty value.
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
    handler.next->settleFulfilled(result.toVariant());
}

} // namespace SynQt
