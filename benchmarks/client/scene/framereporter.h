// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_BENCH_FRAMEREPORTER_H
#define SYNQT_BENCH_FRAMEREPORTER_H

#include <QObject>

// Reports frame-time batches from the QML scene to the browser console, because qWarning reaches
// the WebAssembly console reliably where QML console.log does not in a release build. The client
// frame-time harness reads these lines off the console to build its per-blob-count distribution.
class FrameReporter : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    void report(int blobs, double frameMs)
    {
        qWarning("BENCH blobs=%d frameMs=%.4f", blobs, frameMs);
    }

    void finish()
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        qWarning("BENCH done");
    }

private:
    bool m_finished{false};
};

#endif // SYNQT_BENCH_FRAMEREPORTER_H
