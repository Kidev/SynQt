// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_OTLPEXPORTER_H
#define SYNQT_OTLPEXPORTER_H

#include "ieventexporter.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QUrl>

namespace SynQt {

/// Where the collector is and how to talk to it.
struct OtlpSettings
{
    /// The collector's base URL, the one every OTLP/HTTP receiver publishes:
    /// `http://localhost:4318`. The signal paths are appended, so one setting reaches both
    /// `/v1/logs` and `/v1/traces`.
    QUrl endpoint;
    /// Extra request headers, which in practice means one API key. They come from the
    /// entity's environment (see `headersFromEnvironment`) and never from `synqt.yaml`.
    QHash<QString, QString> headers;
    /// How many requests may be waiting on the collector at once before a batch is dropped
    /// instead of queued.
    int maxInFlight{8};
    /// How long one request may take before it is abandoned.
    int timeoutMs{5000};
};

/// The OTLP/HTTP body for the events that are records. Everything that ended no span.
/// Empty when the batch has none, because an empty POST every batch is a collector asking
/// why it is being woken up.
QJsonObject otlpLogsRequest(const QList<TraceEvent> &events);

/// The OTLP/HTTP body for the events that closed a span. The start time is derived from
/// the end and the duration, which is what the record carries.
QJsonObject otlpTracesRequest(const QList<TraceEvent> &events);

/// OpenTelemetry's severity ladder, which is not SynQt's enum: TRACE is 1, DEBUG 5, INFO
/// 9, WARN 13, ERROR 17, FATAL 21. Exported as the published number so a collector's own
/// filters work without a mapping nobody wrote down.
int otlpSeverityNumber(Severity severity);

/// Whether this collector may be spoken to at all: https anywhere, http only to this machine.
/// A batch is the entity's security record and the request carries the collector's API key.
/// `synqt check` applies the same rule and refuses it outright in a release build.
bool isExportableCollector(const QUrl &endpoint);

/// Ship the same events to an OpenTelemetry collector, over HTTP with JSON encoding (a
/// supported OTLP encoding that needs only QNetworkAccessManager). It hangs off the monitor,
/// never an entity, so a dead collector can only drop its own in-flight batch.
class OtlpExporter : public IEventExporter
{
public:
    explicit OtlpExporter(const OtlpSettings &settings);
    ~OtlpExporter() override;

    QString name() const override;
    void take(const QList<TraceEvent> &events) override;
    qint64 exported() const override;
    qint64 dropped() const override;

    /// The environment variable the headers are read from: one `Key: value` per line.
    static QByteArray headerVariable();
    /// The headers this process was given, or an empty set. A collector's API key is a
    /// credential, so it lives where every other credential in SynQt lives.
    static QHash<QString, QString> headersFromEnvironment();

    /// Whether the configured endpoint was refused (isExportableCollector). A refused
    /// exporter posts nothing and counts every event it is handed as dropped, so the
    /// accounting says the collector is not being fed rather than implying it is.
    bool isRefused() const;

private:
    void post(const QString &signalPath, const QJsonObject &body, qint64 count);

    OtlpSettings m_settings;
    bool m_refused{false};
    QNetworkAccessManager m_network;
    int m_inFlight{0};
    qint64 m_exported{0};
    qint64 m_dropped{0};
};

} // namespace SynQt

#endif // SYNQT_OTLPEXPORTER_H
