// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_GRAPHICSPROBE_H
#define SYNQT_GRAPHICSPROBE_H

namespace SynQt {

/// Whether this platform can give Qt Quick an accelerated scene graph, and the fallback
/// when it cannot.
///
/// Qt selects the raster adaptation itself when the platform integration reports no
/// RhiBasedRendering (qsgcontextplugin.cpp). QWasmIntegration reports that capability without
/// asking the browser, so on WebAssembly a client with no WebGL context reaches qFatal. qFatal
/// is abort(), which sets emscripten's ABORT flag and drops every callback later queued
/// through emscripten_async_call, the first hop of QEventDispatcherWasm::wakeUp(): the page
/// loses its posted-event queue for good.
class GraphicsProbe
{
public:
    /// True when Qt Quick can have an accelerated scene graph here. Probed once.
    static bool hasAcceleratedGraphics();

    /// Select the raster adaptation when there is no accelerated one. Must run before the
    /// first QQuickWindow, and leaves an explicit QT_QUICK_BACKEND alone.
    static void selectBackend();

    /// True when the raster adaptation was selected, here or by the environment.
    ///
    /// On desktop, Qt's own rule can select it without going through selectBackend(), which this
    /// does not see. The runtime net in Graphics does, so undrawable content still says so; only
    /// the per-route guard is missed.
    static bool isSoftwareRendered();
};

} // namespace SynQt

#endif // SYNQT_GRAPHICSPROBE_H
