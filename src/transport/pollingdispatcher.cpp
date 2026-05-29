// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "pollingdispatcher.h"

#include <QByteArray>
#include <QtGlobal>

namespace SynQt {

void preferPollingEventDispatcher()
{
    // Only when unset, so an operator or a container can choose GLib. isSet, not isEmpty:
    // an empty QT_NO_GLIB is Qt's way of saying "use GLib".
    if (qEnvironmentVariableIsSet("QT_NO_GLIB")) {
        return;
    }
    // Outside Linux nothing reads the variable, so it is set unconditionally, without a
    // platform #if.
    qputenv("QT_NO_GLIB", "1");
}

} // namespace SynQt
