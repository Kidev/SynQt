// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ACTINGFOR_H
#define SYNQT_ACTINGFOR_H

#include <QObject>
#include <QPointer>
#include <QVariantMap>

namespace SynQt {

/// Who the entity is acting for while one slot of its own runs.
///
/// A browser reaches the web edge, the edge reaches a service, that service reaches another.
/// The edge is authenticated to the service by its certificate, but only the edge knows the
/// person it is answering.
///
/// So a slot call on a connect point reached over the mesh carries the session the calling
/// entity is acting for. The generated Source helper opens one of these around the owner's
/// implementation of a slot, naming the Caller that slot is answering, and any outbound call
/// the implementation makes reads current() and carries it on. Work that finishes later, in a
/// timer or a continuation, carries no session: the entity is then acting on its own behalf.
/// It still carries the trace, which is the thread's (see TraceScope).
///
/// The storage is per thread, and it nests: the object restores whatever it displaced.
///
/// \sa SynQt::Caller::forwardedSession, SynQt::Caller::assumeSession
class ActingFor
{
public:
    /// Answer for `caller` for as long as this object lives.
    ///
    /// Only the Caller is remembered; what it acts for is asked when an outbound call happens. A
    /// null caller means the entity is acting on its own behalf.
    explicit ActingFor(QObject *caller);
    ~ActingFor();

    ActingFor(const ActingFor &) = delete;
    ActingFor &operator=(const ActingFor &) = delete;

    /// The session to carry on an outbound call made right now, with the trace the work
    /// belongs to (see TraceScope). Empty when there is neither.
    static QVariantMap current();

private:
    static void withTrace(QVariantMap &session);

    QPointer<QObject> m_displaced;
};

} // namespace SynQt

#endif // SYNQT_ACTINGFOR_H
