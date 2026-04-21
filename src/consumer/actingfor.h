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
/// A system is a chain: a browser reaches the web edge, the edge reaches a service, that
/// service reaches another. The edge is authenticated to the service by its certificate,
/// but the person the edge is answering is known only to the edge, and a service two links
/// from the browser would otherwise see a call from "edge" and nothing else.
///
/// So a slot call on a connect point that is reached over the mesh carries one extra value:
/// the session the calling entity is acting for. This class is where that value waits. The
/// generated Source helper opens one of these around the owner's implementation of a slot,
/// naming the Caller that slot is answering; any outbound call the implementation then
/// makes reads current() and carries it on. Work that finishes later, in a timer or a
/// continuation, is outside the object's life and carries nothing, which is right: by then
/// the entity is acting on its own behalf.
///
/// One entity is one event loop, so nothing here is contended; the storage is per thread
/// anyway, so that stays true of a runtime that one day runs a slot somewhere else. It
/// nests: the object restores whatever it displaced.
///
/// \sa SynQt::Caller::forwardedSession, SynQt::Caller::assumeSession
class ActingFor
{
public:
    /// Answer for `caller` for as long as this object lives.
    ///
    /// Only the Caller is remembered here. What it is acting for is asked of it when an
    /// outbound call happens, so an inbound slot that calls nothing out pays
    /// nothing. A null caller means the entity is acting on its own behalf.
    explicit ActingFor(QObject *caller);
    ~ActingFor();

    ActingFor(const ActingFor &) = delete;
    ActingFor &operator=(const ActingFor &) = delete;

    /// The session to carry on an outbound call made right now, empty when there is none.
    static QVariantMap current();

private:
    QPointer<QObject> m_displaced;
};

} // namespace SynQt

#endif // SYNQT_ACTINGFOR_H
