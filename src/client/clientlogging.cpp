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

// Route one message to the browser console by severity (WASM) or stderr (desktop). The
// browser-console path is what makes console.log visible in a release WASM build, where the
// default handler does not surface. QtFatalMsg is emitted here as an error; Qt still aborts
// after the handler returns, so fatal semantics are unchanged.
void routeToConsole(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Q_UNUSED(context)
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

// Production: debug and info (QML console.log / console.info, qDebug) never reach the user;
// warnings and above still route so real problems remain visible.
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
