// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "actingfor.h"

#include <QMetaObject>

namespace SynQt {

namespace {

QPointer<QObject> &acting()
{
    // Per thread, though one entity is one event loop and every slot runs on it. The cost
    // is a thread-local lookup on a path that already crosses the network, and what it buys
    // is that "whose session does an outbound call carry" stops being a fact about how the
    // runtime happens to be scheduled today. A plain static holding that answer is right
    // until the first slot runs somewhere else, and wrong in the direction where one
    // caller's session travels under another caller's call.
    //
    // Not a QObject, so none of the caveat in WebSocketTransport's thread_local applies: a
    // QPointer whose target is gone is already null and destroying it does nothing.
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
        return QVariantMap{};
    }
    // Asked for here rather than on the way in, so an inbound slot that calls nothing out
    // pays nothing for this: the lookup happens only when there is an outbound call to
    // carry the answer. By name, because the generated code that opens an ActingFor has no
    // way to include caller.h, and a Caller is the only thing that answers to this.
    QVariantMap session;
    QMetaObject::invokeMethod(caller, "forwardedSession", Qt::DirectConnection,
                              Q_RETURN_ARG(QVariantMap, session));
    return session;
}

} // namespace SynQt
