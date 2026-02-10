// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "jsonlexporter.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace SynQt {

namespace {

QJsonObject attributeObject(const QVariantMap &attributes)
{
    QJsonObject object;
    for (auto it{attributes.cbegin()}; it != attributes.cend(); ++it) {
        object.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }
    return object;
}

} // namespace

JsonlExporter::JsonlExporter(const QString &path, qint64 maxBytes, int keep)
    : m_path{path}
    , m_maxBytes{maxBytes}
    , m_keep{qMax(0, keep)}
{
    m_file.setFileName(m_path);
}

JsonlExporter::~JsonlExporter()
{
    flush();
}

QString JsonlExporter::name() const
{
    return QStringLiteral("jsonl");
}

qint64 JsonlExporter::exported() const
{
    return m_exported;
}

qint64 JsonlExporter::dropped() const
{
    return m_dropped;
}

QByteArray JsonlExporter::line(const TraceEvent &event)
{
    QJsonObject object;
    object.insert(QStringLiteral("ts"), static_cast<double>(event.timestampMs));
    object.insert(QStringLiteral("severity"), severityName(event.severity));
    object.insert(QStringLiteral("category"), categoryName(event.category));
    object.insert(QStringLiteral("entity"), event.entity);
    object.insert(QStringLiteral("message"), event.message);
    object.insert(QStringLiteral("ok"), event.ok);
    if (!event.traceId.isEmpty()) {
        object.insert(QStringLiteral("traceId"), event.traceId);
    }
    if (!event.spanId.isEmpty()) {
        object.insert(QStringLiteral("spanId"), event.spanId);
    }
    if (!event.parentSpanId.isEmpty()) {
        object.insert(QStringLiteral("parentSpanId"), event.parentSpanId);
    }
    if (event.durationUs >= 0) {
        object.insert(QStringLiteral("durationMs"),
                      static_cast<double>(event.durationUs) / 1000.0);
    }
    if (event.untrusted) {
        object.insert(QStringLiteral("untrusted"), true);
    }
    if (!event.attributes.isEmpty()) {
        object.insert(QStringLiteral("attributes"), attributeObject(event.attributes));
    }
    // Compact, one line per event: readers split on newlines before parsing.
    return QJsonDocument{object}.toJson(QJsonDocument::Compact);
}

bool JsonlExporter::ensureOpen()
{
    if (m_file.isOpen()) {
        return true;
    }
    const QDir parent{QFileInfo{m_path}.absolutePath()};
    if (!parent.exists()) {
        QDir{}.mkpath(parent.absolutePath());
    }
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return false;
    }
    // Counted, not asked of the file: QFile buffers, so its size lags and the cap would
    // overshoot.
    m_written = QFileInfo{m_path}.size();
    return true;
}

void JsonlExporter::rotate()
{
    m_file.close();
    // Oldest first, so nothing is overwritten before being shifted. The file past `keep` is
    // removed, which bounds the set.
    for (int index{m_keep}; index >= 1; --index) {
        const QString older{m_path + QStringLiteral(".%1").arg(index)};
        if (!QFile::exists(older)) {
            continue;
        }
        if (index == m_keep) {
            QFile::remove(older);
            continue;
        }
        QFile::rename(older, m_path + QStringLiteral(".%1").arg(index + 1));
    }
    if (m_keep > 0) {
        QFile::rename(m_path, m_path + QStringLiteral(".1"));
    } else {
        QFile::remove(m_path);
    }
}

void JsonlExporter::take(const QList<TraceEvent> &events)
{
    if (events.isEmpty()) {
        return;
    }
    if (!ensureOpen()) {
        // Counted as dropped: an unwritable path is a configuration mistake that would
        // otherwise look like silence.
        m_dropped += events.size();
        return;
    }
    for (const TraceEvent &event : events) {
        const QByteArray text{line(event)};
        if ((m_maxBytes > 0) && ((m_written + text.size() + 1) > m_maxBytes)) {
            rotate();
            if (!ensureOpen()) {
                m_dropped += 1;
                continue;
            }
        }
        if (m_file.write(text) < 0 || m_file.write("\n") < 0) {
            m_dropped += 1;
            continue;
        }
        m_written += text.size() + 1;
        m_exported += 1;
    }
    flush();
}

void JsonlExporter::flush()
{
    if (m_file.isOpen()) {
        m_file.flush();
    }
}

} // namespace SynQt
