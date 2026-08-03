// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_BENCH_FRAMEREPORTER_H
#define SYNQT_BENCH_FRAMEREPORTER_H

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

// Reports frame-time batches from the QML scene to the browser console, because qWarning reaches
// the WebAssembly console reliably where QML console.log does not in a release build. The client
// frame-time harness reads these lines off the console to build its per-blob-count distribution,
// one sample per frame.
class FrameReporter : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    void report(int blobs, const QList<double> &frameMs)
    {
        QStringList frames;
        frames.reserve(frameMs.size());
        for (const double frame : frameMs) {
            frames.append(QString::number(frame, 'f', 3));
        }
        qWarning("BENCH blobs=%d frames=%s", blobs,
                 qPrintable(frames.join(QLatin1Char(','))));
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
