// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "clientlogging.h"

#include <QByteArray>
#include <QString>

#include <cstdio>

#ifdef Q_OS_WASM
#  include <emscripten/console.h>
#endif

namespace SynQt {

namespace {

// Route one message to the browser console by severity (WASM) or to stderr (desktop). The
// browser console route is what makes console.log visible in a release WASM build.
// QtFatalMsg is logged as an error; Qt still aborts after the handler returns.
void routeToConsole(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Q_UNUSED(context)
    Q_UNUSED(type)  // the severity only picks a console function, and only in the browser
    const QByteArray text{message.toUtf8()};
#ifdef Q_OS_WASM
    switch (type) {
    case QtWarningMsg:
        emscripten_console_warn(text.constData());
        break;
    case QtCriticalMsg:
    case QtFatalMsg:
        emscripten_console_error(text.constData());
        break;
    case QtDebugMsg:
    case QtInfoMsg:
        emscripten_console_log(text.constData());
        break;
    }
#else
    std::fprintf(stderr, "%s\n", text.constData());
#endif
}

// Production: debug and info (console.log, console.info, qDebug) are dropped; warnings and
// above are routed.
void dropDebug(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtDebugMsg || type == QtInfoMsg) {
        return;
    }
    routeToConsole(type, context, message);
}

} // namespace

ClientLogging::Mode ClientLogging::modeFromName(const QString &name)
{
    if (name == QLatin1String("qt")) {
        return Mode::Qt;
    }
    if (name == QLatin1String("none")) {
        return Mode::Silent;
    }
    return Mode::Console;
}

void ClientLogging::install(Mode mode)
{
    switch (mode) {
    case Mode::Console:
        qInstallMessageHandler(routeToConsole);
        break;
    case Mode::Silent:
        qInstallMessageHandler(dropDebug);
        break;
    case Mode::Qt:
        break;   // keep Qt's default handler
    }
}

} // namespace SynQt
