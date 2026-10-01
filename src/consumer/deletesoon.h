// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_DELETESOON_H
#define SYNQT_DELETESOON_H

#include <QObject>

namespace SynQt {

/// Deletes an object once the current call stack has unwound, the way
/// `QObject::deleteLater()` does, but without depending on Qt's posted-event queue.
///
/// On Qt for WebAssembly, `QCoreApplication::postEvent()` calls
/// `QEventDispatcherWasm::wakeUp()`, which arms a zero-delay browser timeout from inside a
/// second zero-delay callback and arms no other while one is pending. If either callback is
/// lost, the queue is never drained again for the life of the page, and every later
/// `deleteLater()` leaves a live, connected object. Timer events are delivered with
/// `QCoreApplication::sendEvent()` by `QTimerInfoList::activateTimers()`, so a zero-delay
/// timer gives the same "after this stack unwinds" guarantee by an independent path.
///
/// See tests/transport-spike/FIREFOX-LINUX.md for the fix in Qt itself. Everywhere but
/// WebAssembly this is `deleteLater()`.
///
/// Passing nullptr is a no-op. Deleting the object by other means first is safe: the pending
/// deletion uses the object as its context and is dropped with it.
void deleteSoon(QObject *object);

} // namespace SynQt

#endif // SYNQT_DELETESOON_H
