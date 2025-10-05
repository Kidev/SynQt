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
    // deletion goes too. QTimer::singleShot() with zero interval delivers through
    // QSingleShotTimer::timerEvent(), a timer event, and the connection is direct, so
    // nothing here uses the posted-event queue.
    QTimer::singleShot(0, object, [object]() { delete object; });
#else
    object->deleteLater();
#endif
}

} // namespace SynQt
