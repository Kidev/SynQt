// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The cold tier. Handing the same events to whatever the operator already runs.
//
// Two things have to hold. The encoding has to be the one OpenTelemetry publishes, field
// name for field name, because "nearly OTLP" is a format with no consumers. And a
// collector that is down has to be the collector's problem. The monitor keeps its history
// and keeps answering its console whether or not anything is listening at the other end.

#include "eventstore.h"
#include "jsonlexporter.h"
#include "monitorservice.h"
#include "operatorstore.h"
#include "otlpexporter.h"
#include "traceevent.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace SynQt;

namespace {

TraceEvent record(const QString &entity, const QString &message)
{
    TraceEvent event;
    event.timestampMs = 1750000000123LL;
    event.severity = Severity::Info;
    event.category = Category::Application;
    event.entity = entity;
    event.message = message;
    event.attributes.insert(QStringLiteral("member"), QStringLiteral("placeBid"));
    event.attributes.insert(QStringLiteral("args"), 2);
    event.attributes.insert(QStringLiteral("ok"), true);
    return event;
}

TraceEvent span(const QString &entity, const QString &name)
{
    TraceEvent event{record(entity, name)};
    event.category = Category::Call;
    event.traceId = QStringLiteral("4bf92f3577b34da6a3ce929d0e0e4736");
    event.spanId = QStringLiteral("00f067aa0ba902b7");
    event.parentSpanId = QStringLiteral("0102030405060708");
    event.durationUs = 4500;
    return event;
}

/// The one entry under `key` in an OTLP attribute array, which is a list and not a map.
QJsonObject attributeNamed(const QJsonArray &attributes, const QString &key)
{
    for (const QJsonValue &value : attributes) {
        if (value.toObject().value(QStringLiteral("key")).toString() == key) {
            return value.toObject();
        }
    }
    return QJsonObject{};
}

} // namespace

class TestExport : public QObject
{
    Q_OBJECT

private slots:
    void aLogRecordCarriesTheFieldNamesTheSpecificationPublishes()
    {
        const QJsonObject body{otlpLogsRequest({record(QStringLiteral("web"),
                                                       QStringLiteral("bid accepted"))})};
        QJsonArray resources;
        resources = body.value(QStringLiteral("resourceLogs")).toArray();
        QCOMPARE(resources.size(), 1);
        const QJsonObject resource{resources.first().toObject()};

        // The entity is the OpenTelemetry resource, because that is what a collector
        // groups by and what every dashboard already has a filter for.
        // '=' and not '{}' here and below: QJsonArray has an initializer-list
        // constructor, so brace-init would build an array holding the array.
        QJsonArray resourceAttributes;
        resourceAttributes = resource.value(QStringLiteral("resource")).toObject()
                                 .value(QStringLiteral("attributes")).toArray();
        QCOMPARE(attributeNamed(resourceAttributes, QStringLiteral("service.name"))
                     .value(QStringLiteral("value")).toObject()
                     .value(QStringLiteral("stringValue")).toString(),
                 QStringLiteral("web"));

        QJsonArray scopes;
        scopes = resource.value(QStringLiteral("scopeLogs")).toArray();
        QCOMPARE(scopes.size(), 1);
        QJsonArray records;
        records = scopes.first().toObject().value(QStringLiteral("logRecords")).toArray();
        QCOMPARE(records.size(), 1);
        const QJsonObject entry{records.first().toObject()};

        // Nanoseconds, as a string. The field is a 64-bit fixed integer and proto3's JSON
        // mapping writes those as strings, so a collector reading a number rejects it.
        QCOMPARE(entry.value(QStringLiteral("timeUnixNano")).toString(),
                 QStringLiteral("1750000000123000000"));
        QCOMPARE(entry.value(QStringLiteral("observedTimeUnixNano")).toString(),
                 QStringLiteral("1750000000123000000"));
        QCOMPARE(entry.value(QStringLiteral("severityNumber")).toInt(), 9);
        QCOMPARE(entry.value(QStringLiteral("severityText")).toString(),
                 QStringLiteral("info"));
        QCOMPARE(entry.value(QStringLiteral("body")).toObject()
                     .value(QStringLiteral("stringValue")).toString(),
                 QStringLiteral("bid accepted"));

        // The category is an attribute rather than part of the body, so a collector can
        // filter on it without parsing a sentence.
        QJsonArray attributes;
        attributes = entry.value(QStringLiteral("attributes")).toArray();
        QCOMPARE(attributeNamed(attributes, QStringLiteral("synqt.category"))
                     .value(QStringLiteral("value")).toObject()
                     .value(QStringLiteral("stringValue")).toString(),
                 QStringLiteral("application"));
    }

    void everyAttributeTypeSurvivesAsItsOwnAnyValue()
    {
        const QJsonObject body{otlpLogsRequest({record(QStringLiteral("web"),
                                                       QStringLiteral("bid accepted"))})};
        QJsonArray attributes;
        attributes = body.value(QStringLiteral("resourceLogs")).toArray().first().toObject()
                         .value(QStringLiteral("scopeLogs")).toArray().first().toObject()
                         .value(QStringLiteral("logRecords")).toArray().first().toObject()
                         .value(QStringLiteral("attributes")).toArray();

        QCOMPARE(attributeNamed(attributes, QStringLiteral("member"))
                     .value(QStringLiteral("value")).toObject()
                     .value(QStringLiteral("stringValue")).toString(),
                 QStringLiteral("placeBid"));
        // An integer is an intValue, and proto3 writes a 64-bit one as a string. A number
        // that arrived as a number and left as "2" would be a number no dashboard can
        // aggregate.
        QCOMPARE(attributeNamed(attributes, QStringLiteral("args"))
                     .value(QStringLiteral("value")).toObject()
                     .value(QStringLiteral("intValue")).toString(),
                 QStringLiteral("2"));
        QCOMPARE(attributeNamed(attributes, QStringLiteral("ok"))
                     .value(QStringLiteral("value")).toObject()
                     .value(QStringLiteral("boolValue")).toBool(),
                 true);
    }

    void theSeverityLadderIsOpenTelemetrysAndNotOurOwn()
    {
        QCOMPARE(otlpSeverityNumber(Severity::Trace), 1);
        QCOMPARE(otlpSeverityNumber(Severity::Debug), 5);
        QCOMPARE(otlpSeverityNumber(Severity::Info), 9);
        QCOMPARE(otlpSeverityNumber(Severity::Warning), 13);
        QCOMPARE(otlpSeverityNumber(Severity::Error), 17);
        QCOMPARE(otlpSeverityNumber(Severity::Fatal), 21);
    }

    void anEventThatClosedASpanExportsAsASpanWithItsParentIntact()
    {
        const QJsonObject body{otlpTracesRequest({span(QStringLiteral("web"),
                                                       QStringLiteral("placeBid"))})};
        QJsonArray spans;
        spans = body.value(QStringLiteral("resourceSpans")).toArray().first().toObject()
                    .value(QStringLiteral("scopeSpans")).toArray().first().toObject()
                    .value(QStringLiteral("spans")).toArray();
        QCOMPARE(spans.size(), 1);
        const QJsonObject entry{spans.first().toObject()};

        QCOMPARE(entry.value(QStringLiteral("traceId")).toString(),
                 QStringLiteral("4bf92f3577b34da6a3ce929d0e0e4736"));
        QCOMPARE(entry.value(QStringLiteral("spanId")).toString(),
                 QStringLiteral("00f067aa0ba902b7"));
        // Without this one link a trace is a pile of unrelated spans, which is the whole
        // difference between "here is what that click did" and a log file.
        QCOMPARE(entry.value(QStringLiteral("parentSpanId")).toString(),
                 QStringLiteral("0102030405060708"));
        QCOMPARE(entry.value(QStringLiteral("name")).toString(), QStringLiteral("placeBid"));
        QCOMPARE(entry.value(QStringLiteral("endTimeUnixNano")).toString(),
                 QStringLiteral("1750000000123000000"));
        // The record carries when the span ended and how long it took, so the start is
        // derived rather than stored, and 4500us before the end is where it must land.
        QCOMPARE(entry.value(QStringLiteral("startTimeUnixNano")).toString(),
                 QStringLiteral("1750000000118500000"));
        QCOMPARE(entry.value(QStringLiteral("status")).toObject()
                     .value(QStringLiteral("code")).toInt(), 1);
    }

    void aRefusedCallIsAnErrorSpanAndNotAMissingOne()
    {
        TraceEvent refused{span(QStringLiteral("web"), QStringLiteral("placeBid"))};
        refused.ok = false;
        refused.severity = Severity::Warning;
        refused.attributes.insert(QStringLiteral("outcome"), QStringLiteral("refused"));

        const QJsonObject entry{
            otlpTracesRequest({refused}).value(QStringLiteral("resourceSpans")).toArray()
                .first().toObject().value(QStringLiteral("scopeSpans")).toArray()
                .first().toObject().value(QStringLiteral("spans")).toArray()
                .first().toObject()};
        QCOMPARE(entry.value(QStringLiteral("status")).toObject()
                     .value(QStringLiteral("code")).toInt(), 2);
    }

    void anEventThatEndedNoSpanIsALogAndNeverASpan()
    {
        const QList<TraceEvent> batch{record(QStringLiteral("web"),
                                             QStringLiteral("started")),
                                      span(QStringLiteral("web"),
                                           QStringLiteral("placeBid"))};

        // A span exported as a log too would double every call in a collector's count.
        QCOMPARE(otlpLogsRequest(batch).value(QStringLiteral("resourceLogs")).toArray()
                     .first().toObject().value(QStringLiteral("scopeLogs")).toArray()
                     .first().toObject().value(QStringLiteral("logRecords")).toArray()
                     .size(),
                 1);
        QCOMPARE(otlpTracesRequest(batch).value(QStringLiteral("resourceSpans")).toArray()
                     .first().toObject().value(QStringLiteral("scopeSpans")).toArray()
                     .first().toObject().value(QStringLiteral("spans")).toArray().size(),
                 1);
    }

    void twoEntitiesAreTwoResourcesAndNotOneMixedStream()
    {
        QJsonArray resources;
        resources = otlpLogsRequest({record(QStringLiteral("web"), QStringLiteral("a")),
                                     record(QStringLiteral("database"), QStringLiteral("b")),
                                     record(QStringLiteral("web"), QStringLiteral("c"))})
                        .value(QStringLiteral("resourceLogs")).toArray();
        QCOMPARE(resources.size(), 2);

        QStringList named;
        for (const QJsonValue &value : resources) {
            named.append(attributeNamed(value.toObject().value(QStringLiteral("resource"))
                                            .toObject()
                                            .value(QStringLiteral("attributes")).toArray(),
                                        QStringLiteral("service.name"))
                             .value(QStringLiteral("value")).toObject()
                             .value(QStringLiteral("stringValue")).toString());
        }
        named.sort();
        QCOMPARE(named, QStringList({QStringLiteral("database"), QStringLiteral("web")}));
    }

    void nothingIsSentWhenThereIsNothingToSend()
    {
        // An entity whose whole batch was log records must not produce an empty traces
        // request, and the other way round. An empty POST every batch is a collector
        // asking why it is being woken up.
        QVERIFY(otlpTracesRequest({record(QStringLiteral("web"), QStringLiteral("a"))})
                    .isEmpty());
        QVERIFY(otlpLogsRequest({span(QStringLiteral("web"), QStringLiteral("a"))})
                    .isEmpty());
        QVERIFY(otlpLogsRequest({}).isEmpty());
    }

    void jsonlIsOneObjectPerLineAndNotAPrettyPrintedDocument()
    {
        QTemporaryDir dir;
        const QString path{dir.filePath(QStringLiteral("events.jsonl"))};
        {
            JsonlExporter exporter{path, 0, 3};
            exporter.take({record(QStringLiteral("web"), QStringLiteral("first")),
                           span(QStringLiteral("database"), QStringLiteral("second"))});
            exporter.flush();
            QCOMPARE(exporter.exported(), 2);
        }

        QFile file{path};
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines{file.readAll().trimmed().split('\n')};
        QCOMPARE(lines.size(), 2);

        QJsonParseError error;
        const QJsonObject first{QJsonDocument::fromJson(lines.at(0), &error).object()};
        QCOMPARE(error.error, QJsonParseError::NoError);
        QCOMPARE(first.value(QStringLiteral("entity")).toString(), QStringLiteral("web"));
        QCOMPARE(first.value(QStringLiteral("severity")).toString(),
                 QStringLiteral("info"));
        QCOMPARE(first.value(QStringLiteral("message")).toString(),
                 QStringLiteral("first"));
        // Milliseconds since the epoch, the field name every line-shipper's timestamp
        // parser is pointed at.
        QCOMPARE(static_cast<qint64>(first.value(QStringLiteral("ts")).toDouble()),
                 1750000000123LL);

        const QJsonObject second{QJsonDocument::fromJson(lines.at(1)).object()};
        QCOMPARE(second.value(QStringLiteral("traceId")).toString(),
                 QStringLiteral("4bf92f3577b34da6a3ce929d0e0e4736"));
        QCOMPARE(second.value(QStringLiteral("attributes")).toObject()
                     .value(QStringLiteral("member")).toString(),
                 QStringLiteral("placeBid"));
    }

    void jsonlRotatesAtItsCapAndKeepsABoundedNumberOfFiles()
    {
        QTemporaryDir dir;
        const QString path{dir.filePath(QStringLiteral("events.jsonl"))};
        {
            JsonlExporter exporter{path, 4096, 2};
            for (int round{0}; round < 400; ++round) {
                exporter.take({record(QStringLiteral("web"),
                                      QStringLiteral("event %1").arg(round))});
            }
            exporter.flush();
        }

        // The live file plus exactly `keep` rotations, and nothing beyond: a monitor that
        // fills the disk it is watching has become the outage.
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(path + QStringLiteral(".1")));
        QVERIFY(QFileInfo::exists(path + QStringLiteral(".2")));
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".3")));
        QVERIFY(QFileInfo{path}.size() <= 4096 + 1024);

        // The newest is in the live file, because that is the one an operator tails.
        QFile file{path};
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray text{file.readAll().trimmed()};
        const QJsonObject last{
            QJsonDocument::fromJson(text.mid(text.lastIndexOf('\n') + 1)).object()};
        QCOMPARE(last.value(QStringLiteral("message")).toString(),
                 QStringLiteral("event 399"));
    }

    void aCollectorThatIsDownCostsTheMonitorNothingAndLosesNoHistory()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY2(store.open(), qPrintable(store.errorString()));
        OperatorStore operators;

        OtlpSettings settings;
        // Port 1 on loopback. Nothing is listening, which is what a collector being down
        // looks like from here.
        settings.endpoint = QUrl{QStringLiteral("http://127.0.0.1:1")};
        OtlpExporter exporter{settings};

        MonitorService service{&store, &operators, MonitorService::Retention{}};
        service.addExporter(&exporter);

        QVariantList batch;
        for (int index{0}; index < 200; ++index) {
            batch.append(record(QStringLiteral("claimed"),
                                QStringLiteral("event %1").arg(index)).toVariant());
        }

        QElapsedTimer clock;
        clock.start();
        service.take(batch, QStringLiteral("web"));
        const qint64 elapsed{clock.elapsed()};

        // The history is what the monitor is for. The exporter is a convenience for
        // whoever already runs a collector. A dead collector may not cost the first one.
        QCOMPARE(store.count(), 200);
        QCOMPARE(static_cast<int>(service.stored()), 200);
        QVERIFY2(elapsed < 500, qPrintable(QStringLiteral("take() blocked for %1 ms")
                                               .arg(elapsed)));
    }

    void anExporterNeverQueuesWithoutABoundAndSaysWhatItGaveUpOn()
    {
        OtlpSettings settings;
        settings.endpoint = QUrl{QStringLiteral("http://127.0.0.1:1")};
        settings.maxInFlight = 2;
        OtlpExporter exporter{settings};

        for (int round{0}; round < 50; ++round) {
            exporter.take({record(QStringLiteral("web"),
                                  QStringLiteral("event %1").arg(round))});
        }

        // Unbounded buffering in front of a collector that is not answering is how a
        // monitor runs a machine out of memory. Dropping is the right answer. Dropping
        // quietly is not.
        QVERIFY2(exporter.dropped() > 0, "an unreachable collector queued without a bound");
        QCOMPARE(exporter.exported() + exporter.dropped(), 50);
    }

    // The path a collector is asked for is the signal's own, under whatever base path the
    // endpoint names, however the endpoint was written. A trailing slash on the base is
    // how most people write a URL, and `//v1/logs` is a path no collector serves.
    void theSignalPathIsJoinedToTheEndpointWithOneSlash_data()
    {
        QTest::addColumn<QString>("base");
        QTest::addColumn<QString>("expected");
        QTest::newRow("bare") << QString{} << QStringLiteral("/v1/logs");
        QTest::newRow("trailing slash") << QStringLiteral("/") << QStringLiteral("/v1/logs");
        QTest::newRow("base path") << QStringLiteral("/otlp") << QStringLiteral("/otlp/v1/logs");
        QTest::newRow("base path, trailing slash")
            << QStringLiteral("/otlp/") << QStringLiteral("/otlp/v1/logs");
    }

    void theSignalPathIsJoinedToTheEndpointWithOneSlash()
    {
        QFETCH(QString, base);
        QFETCH(QString, expected);

        QTcpServer collector;
        QVERIFY(collector.listen(QHostAddress::LocalHost));
        QByteArray requestLine;
        connect(&collector, &QTcpServer::newConnection, &collector, [&collector, &requestLine]() {
            QTcpSocket *socket{collector.nextPendingConnection()};
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &requestLine]() {
                if (requestLine.isEmpty() && socket->canReadLine()) {
                    requestLine = socket->readLine().trimmed();
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
                }
            });
        });

        OtlpSettings settings;
        settings.endpoint = QUrl{QStringLiteral("http://127.0.0.1:%1%2")
                                     .arg(collector.serverPort())
                                     .arg(base)};
        OtlpExporter exporter{settings};
        exporter.take({record(QStringLiteral("web"), QStringLiteral("placeBid"))});

        QTRY_VERIFY(!requestLine.isEmpty());
        QCOMPARE(requestLine, QByteArrayLiteral("POST ") + expected.toLatin1()
                                  + QByteArrayLiteral(" HTTP/1.1"));
    }

    void aPlaintextCollectorOnAnotherHostIsRefusedAndNotQuietlyFed()
    {
        // A batch is the record of every refusal, every caller and every peer, and the
        // request carrying it carries the collector's API key. Over http to another
        // machine that is a plaintext feed of the system's security events, so the
        // exporter refuses the endpoint rather than posting to it. Refused loudly: the
        // events are counted as dropped, so the monitor's own accounting says the cold
        // tier is not being fed.
        OtlpSettings settings;
        settings.endpoint = QUrl{QStringLiteral("http://collector.internal:4318")};
        QTest::ignoreMessage(QtCriticalMsg,
                             "SynQt: refusing to export to 'http://collector.internal:4318': "
                             "OTLP over plaintext http to a host that is not this one puts "
                             "every event and the collector's API key on the network in the "
                             "clear. Use https, or a collector on localhost.");
        OtlpExporter exporter{settings};
        QVERIFY(exporter.isRefused());

        exporter.take({record(QStringLiteral("web"), QStringLiteral("placeBid")),
                       record(QStringLiteral("web"), QStringLiteral("closeLot"))});
        QCOMPARE(exporter.exported(), 0);
        QCOMPARE(exporter.dropped(), 2);
    }

    void httpsAnywhereAndPlaintextOnlyToThisMachine()
    {
        // A collector on localhost or in the same pod is the ordinary deployment and its
        // traffic never reaches a network, so it is not refused.
        QVERIFY(isExportableCollector(QUrl{QStringLiteral("https://api.honeycomb.io")}));
        QVERIFY(isExportableCollector(QUrl{QStringLiteral("http://127.0.0.1:4318")}));
        QVERIFY(isExportableCollector(QUrl{QStringLiteral("http://localhost:4318")}));
        QVERIFY(isExportableCollector(QUrl{QStringLiteral("http://[::1]:4318")}));

        QVERIFY(!isExportableCollector(QUrl{QStringLiteral("http://collector.internal:4318")}));
        QVERIFY(!isExportableCollector(QUrl{QStringLiteral("http://10.0.0.9:4318")}));
        // Not a scheme this exports over, so not one to treat as safe by omission.
        QVERIFY(!isExportableCollector(QUrl{QStringLiteral("ftp://127.0.0.1/")}));
    }

    void headersComeFromTheEnvironmentAndNeverFromTheProjectFile()
    {
        qputenv(OtlpExporter::headerVariable(),
                QByteArray{"x-honeycomb-team: abcd1234\nx-dataset: synqt\n"});
        const QHash<QString, QString> headers{OtlpExporter::headersFromEnvironment()};
        QCOMPARE(headers.size(), 2);
        QCOMPARE(headers.value(QStringLiteral("x-honeycomb-team")),
                 QStringLiteral("abcd1234"));
        QCOMPARE(headers.value(QStringLiteral("x-dataset")), QStringLiteral("synqt"));
        qunsetenv(OtlpExporter::headerVariable());

        // A collector's API key is a credential, so it lives where every other credential
        // in SynQt lives. The entity's environment, never `synqt.yaml`.
        QVERIFY(OtlpExporter::headersFromEnvironment().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestExport)
#include "tst_export.moc"
