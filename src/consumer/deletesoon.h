// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_DELETESOON_H
#define SYNQT_DELETESOON_H

#include <QObject>

namespace SynQt {

/// Deletes an object once the current call stack has unwound, the way
/// `QObject::deleteLater()` does, but without depending on Qt's posted-event
/// queue.
///
/// On Qt for WebAssembly that queue has exactly one delivery path.
/// `QCoreApplication::postEvent()` calls `QEventDispatcherWasm::wakeUp()`,
/// which arms a zero-delay browser timeout from inside a second zero-delay
/// browser callback, and it does not arm another while one is pending. If
/// either callback is lost, the queue is never drained again for the life of
/// the page, because the flag that would let it re-arm is cleared only by the
/// callback that never came. Every `deleteLater()` issued afterwards is a
/// leak, and the object stays live and connected rather than merely
/// unreclaimed. Timer events do not share that fate:
/// `QTimerInfoList::activateTimers()` delivers them with
/// `QCoreApplication::sendEvent()`, so a zero-delay timer gets the same
/// "after this stack unwinds" guarantee from an independent path.
///
/// See tests/m0-transport/FIREFOX-LINUX.md for the measurement and for the
/// fix in Qt itself. Everywhere but WebAssembly this is `deleteLater()`.
///
/// Passing nullptr is a no-op. Deleting the object by other means first is
/// safe: the pending deletion is bound to the object as its context, so it is
/// dropped along with it.
void deleteSoon(QObject *object);

} // namespace SynQt

#endif // SYNQT_DELETESOON_H
