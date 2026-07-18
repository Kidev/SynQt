// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The client frame-time scene: a 2D field of blobs that moves and interpolates every frame, so a
// single build measures how frame cost grows with the number of entities in view. The blob ceiling
// and ramp duration come from the environment (baked into the WASM page at build time by
// run-bench.sh), so a sweep needs no code change; the FrameReporter carries per-frame batches to
// the console for the Playwright harness to collect.

#include "framereporter.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include <QByteArray>

// The blob ceiling and ramp are baked at build time, because WebAssembly has no process
// environment to read at runtime; run-bench.sh sets them per sweep at configure time. Native runs
// may still override them through the environment for quick local checks.
#ifndef SYNQT_BENCH_BLOBS_DEFAULT
#define SYNQT_BENCH_BLOBS_DEFAULT 800
#endif
#ifndef SYNQT_BENCH_RAMP_DEFAULT
#define SYNQT_BENCH_RAMP_DEFAULT 12
#endif

namespace {

int envInt(const char *name, int fallback)
{
    const QByteArray raw{qgetenv(name)};
    if (raw.isEmpty()) {
        return fallback;
    }
    bool ok{false};
    const int value{raw.toInt(&ok)};
    return (ok && value > 0) ? value : fallback;
}

double envDouble(const char *name, double fallback)
{
    const QByteArray raw{qgetenv(name)};
    if (raw.isEmpty()) {
        return fallback;
    }
    bool ok{false};
    const double value{raw.toDouble(&ok)};
    return (ok && value > 0.0) ? value : fallback;
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app{argc, argv};

    const int maxBlobs{envInt("SYNQT_BENCH_BLOBS", SYNQT_BENCH_BLOBS_DEFAULT)};
    const double rampSeconds{envDouble("SYNQT_BENCH_RAMP", double(SYNQT_BENCH_RAMP_DEFAULT))};

    QQmlApplicationEngine engine;

    FrameReporter reporter;
    engine.rootContext()->setContextProperty(QStringLiteral("Bench"), &reporter);
    engine.setInitialProperties({{QStringLiteral("maxBlobs"), maxBlobs},
                                 {QStringLiteral("rampSeconds"), rampSeconds}});

    engine.loadFromModule("synqtclientbench", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    return app.exec();
}
