// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_JSONLEXPORTER_H
#define SYNQT_JSONLEXPORTER_H

#include "ieventexporter.h"

#include <QFile>

namespace SynQt {

/// One JSON object per line, in a file that rotates.
///
/// Promtail, Vector, Filebeat and Fluent Bit all tail a file of JSON lines with no parser to
/// write, and a file works when the collector is on a network the monitor cannot reach.
///
/// The live file is capped, older ones are numbered, and there are never more than `keep` of
/// them, so the exporter cannot fill the disk of the machine it watches.
///
/// A line carries the same record the store holds, so nothing appears here that would not
/// appear in the console.
class JsonlExporter : public IEventExporter
{
public:
    /// `maxBytes` at 0 rotates never, which is only right when something else is rotating
    /// the file. `keep` is how many rotations survive, oldest deleted first.
    JsonlExporter(const QString &path, qint64 maxBytes, int keep);
    ~JsonlExporter() override;

    QString name() const override;
    void take(const QList<TraceEvent> &events) override;
    qint64 exported() const override;
    qint64 dropped() const override;

    /// Push what is buffered to the file, so a reader tailing it sees the last line. Called
    /// on each batch anyway. Exposed because a test asserting on a file has to be able to
    /// say when.
    void flush();

    /// One event as the line that is written for it, without writing it. The shape is the
    /// stored record's own field names, so a query against a shipper's index and a query
    /// against the console are asking for the same things.
    static QByteArray line(const TraceEvent &event);

private:
    bool ensureOpen();
    void rotate();

    QString m_path;
    qint64 m_maxBytes{0};
    int m_keep{0};
    QFile m_file;
    qint64 m_written{0};
    qint64 m_exported{0};
    qint64 m_dropped{0};
};

} // namespace SynQt

#endif // SYNQT_JSONLEXPORTER_H
