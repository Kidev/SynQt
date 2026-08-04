// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "graphicsprobe.h"

#include <QByteArray>
#include <QString>
#include <QtEnvironmentVariables>

#ifdef Q_OS_WASM
#  include <emscripten/val.h>

#  include <string>
#endif

namespace SynQt {

namespace {

const char *backendVariable()
{
    return "QT_QUICK_BACKEND";
}

const char *softwareBackend()
{
    return "software";
}

#ifdef Q_OS_WASM

/// Ask the browser for a WebGL context on a throwaway canvas.
///
/// Through Embind rather than emscripten_run_script, which uses eval() and the edge's
/// Content-Security-Policy refuses it. Same rule as the generated main's edge-URL read.
bool browserHasWebGl()
{
    const emscripten::val document{emscripten::val::global("document")};
    if (document.isUndefined() || document.isNull()) {
        return false;
    }
    emscripten::val canvas{
        document.call<emscripten::val>("createElement", std::string{"canvas"})};
    emscripten::val context{
        canvas.call<emscripten::val>("getContext", std::string{"webgl2"})};
    if (context.isUndefined() || context.isNull()) {
        context = canvas.call<emscripten::val>("getContext", std::string{"webgl"});
    }
    if (context.isUndefined() || context.isNull()) {
        return false;
    }
    // Hand it straight back: a browser allows a small number of live contexts, and this
    // one exists only to answer the question.
    const emscripten::val extension{
        context.call<emscripten::val>("getExtension", std::string{"WEBGL_lose_context"})};
    if (!extension.isUndefined() && !extension.isNull()) {
        extension.call<void>("loseContext");
    }
    return true;
}

#endif // Q_OS_WASM

} // namespace

bool GraphicsProbe::hasAcceleratedGraphics()
{
#ifdef Q_OS_WASM
    static const bool accelerated{browserHasWebGl()};
    return accelerated;
#else
    // Every other platform plugin answers hasCapability(RhiBasedRendering) honestly, so
    // Qt's own selection already covers them.
    return true;
#endif
}

void GraphicsProbe::selectBackend()
{
    if (qEnvironmentVariableIsSet(backendVariable())) {
        return;
    }
    if (hasAcceleratedGraphics()) {
        return;
    }
    qputenv(backendVariable(), QByteArray{softwareBackend()});
}

bool GraphicsProbe::isSoftwareRendered()
{
    return qEnvironmentVariable(backendVariable())
        == QLatin1String(softwareBackend());
}

} // namespace SynQt
