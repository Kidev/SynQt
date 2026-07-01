// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "actingfor.h"

#include "tracescope.h"

#include <QMetaObject>

namespace SynQt {

namespace {

QPointer<QObject> &acting()
{
    // Per thread, although one entity runs its slots on one event loop: the cost is a
    // thread-local lookup on a path that already crosses the network, and it keeps the
    // answer correct if a slot ever runs elsewhere. A plain static would let one caller's
    // session travel under another's call.
    //
    // Not a QObject, so WebSocketTransport's thread_local caveat does not apply: a QPointer
    // to a destroyed target is already null.
    static thread_local QPointer<QObject> caller;
    return caller;
}

} // namespace

ActingFor::ActingFor(QObject *caller)
    : m_displaced{acting()}
{
    acting() = caller;
}

ActingFor::~ActingFor()
{
    acting() = m_displaced;
}

QVariantMap ActingFor::current()
{
    QObject *caller{acting().data()};
    if (!caller) {
        QVariantMap session;
        withTrace(session);
        return session;
    }
    // Looked up here, only when there is an outbound call, so a slot that calls nothing out
    // pays nothing. By name, because the generated code that opens an ActingFor cannot
    // include caller.h.
    QVariantMap session;
    QMetaObject::invokeMethod(caller, "forwardedSession", Qt::DirectConnection,
                              Q_RETURN_ARG(QVariantMap, session));
    withTrace(session);
    return session;
}

void ActingFor::withTrace(QVariantMap &session)
{
    // The trace rides with the session, which already travels down the chain. It is not
    // part of the session: nothing authorizes by it, and a Caller that ignores the map (a
    // browser's) ignores it too. Taken from the thread, not the caller, because a
    // continuation acts for nobody and still belongs to the click that started it.
    TraceScope::current().writeTo(session);
}

} // namespace SynQt
