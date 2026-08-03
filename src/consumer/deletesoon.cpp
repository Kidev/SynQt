// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "deletesoon.h"

#ifdef Q_OS_WASM
#include <QTimer>
#endif

namespace SynQt {

void deleteSoon(QObject *object)
{
    if (!object) {
        return;
    }
#ifdef Q_OS_WASM
    // The object is its own context, so if something else destroys it first the pending
    // deletion goes with it. QTimer::singleShot() with a zero interval delivers through
    // QSingleShotTimer::timerEvent(), which is a timer event and not a posted one, and the
    // connection to a receiver on this thread is direct: no part of this path touches the
    // posted-event queue.
    QTimer::singleShot(0, object, [object]() { delete object; });
#else
    object->deleteLater();
#endif
}

} // namespace SynQt
