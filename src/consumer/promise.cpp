// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "promise.h"

#include <QJSEngine>
#include <QTimer>

#include <QtRemoteObjects/QRemoteObjectPendingCallWatcher>

namespace SynQt {

Promise::Promise(QJSEngine *engine, QObject *parent)
    : QObject{parent}
    , m_engine{engine}
{
}

Promise::Promise(const QRemoteObjectPendingCall &call, QJSEngine *engine, QObject *parent)
    : QObject{parent}
    , m_engine{engine}
{
    if (call.isFinished()) {
        settleFromCall(call);
        return;
    }
    QRemoteObjectPendingCallWatcher *watcher{new QRemoteObjectPendingCallWatcher{call, this}};
    connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
            [this](QRemoteObjectPendingCallWatcher *finished) {
                settleFromCall(*finished);
                finished->deleteLater();
            });

#ifdef Q_OS_WASM
    // On firefox-for-WebAssembly the emscripten posted-event pump that delivers the watcher's
    // queued finished() signal can be starved, so a returning-slot reply resolves (isFinished()
    // becomes true, its value set synchronously in QtRO's notifyAboutReply) yet the watcher never
    // fires and the promise would never settle. Poll the call's own resolved state as a fallback so
    // the promise settles regardless of the queued signal. settleFulfilled/settleRejected are
    // Pending-guarded, so whichever path fires first wins and the other is a no-op. Gated to WASM:
    // native builds drain posted events normally and need no poll.
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
}

void Promise::dispatch(const Handler &handler)
{
    // A fulfilled value flows through an onFulfilled handler; a rejection flows through an
    // onRejected handler (which recovers the chain) and otherwise propagates unchanged.
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
        handler.next->settleFulfilled(m_value);
        return;
    }
    QJSValue argument{m_engine != nullptr
                          ? (m_state == State::Fulfilled ? m_engine->toScriptValue(m_value)
                                                         : m_engine->toScriptValue(m_reason))
                          : QJSValue{}};
    const QJSValue result{callback.call(QJSValueList{argument})};
    if (result.isError()) {
        handler.next->settleRejected(result.toString());
        return;
    }
    handler.next->settleFulfilled(result.toVariant());
}

} // namespace SynQt
