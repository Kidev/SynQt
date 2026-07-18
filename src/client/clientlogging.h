// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CLIENTLOGGING_H
#define SYNQT_CLIENTLOGGING_H

#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QString;
QT_END_NAMESPACE

namespace SynQt {

/// Controls where a client build's diagnostic output goes. Qt's default message handler does
/// not surface to the browser console in a release WebAssembly build, so QML console.log (and
/// qDebug) silently vanish there while the app still runs; which is why headless evidence
/// has to come from qWarning. This routes it deterministically instead, and lets a release
/// build drop debug output so it never leaks to end users. Selected by build.client_logging
/// (console|qt|none); the generated client main installs the chosen mode before the engine
/// loads. A no-op on the Qt default is one branch, so this adds nothing when unused.
class ClientLogging
{
public:
    enum class Mode {
        Console, ///< route every message to the browser console (WASM) or stderr (desktop)
        Qt,      ///< leave Qt's default handler untouched
        Silent,  ///< drop debug and info; keep warnings and above (no console.log in production)
    };

    /// Map a build.client_logging value to a Mode. Unknown or empty falls back to Console so
    /// a misconfiguration is visible (logs appear) rather than silent.
    static Mode modeFromName(const QString &name);

    /// Install the message handler for the given mode. Call once, before the QML engine loads.
    /// Console and Silent install a handler; Qt keeps the default (no handler installed).
    static void install(Mode mode);
};

} // namespace SynQt

#endif // SYNQT_CLIENTLOGGING_H
