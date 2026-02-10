// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MONITORSERVICE_H
#define SYNQT_MONITORSERVICE_H

#include "eventstore.h"
#include "operatorstore.h"

#include <QHash>
#include <QObject>
#include <QVariantList>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace SynQt {

/// The monitor's engine. What its two Sources bridge to.
///
/// The `Ingest` Source hands it batches, and the `Console` Source asks it questions. Both go
/// through here rather than touching the store, so there is one place that decides what a
/// reporting entity is allowed to say about itself and one place that turns a stored record
/// into something a console can show.
///
/// The rule that matters is in `take`: the entity name on every event is the one the
/// transport verified, not the one the batch carried. A reporting entity is authenticated
/// by its certificate, so it cannot report as another entity, and an operator reading the
/// record is reading who said it.
class MonitorService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double received READ received NOTIFY countersChanged)
    Q_PROPERTY(double stored READ stored NOTIFY countersChanged)
    Q_PROPERTY(double dropped READ dropped NOTIFY countersChanged)

public:
    /// How much history to keep. Applied on a timer, because a monitor left running is a
    /// monitor whose store grows until the disk is full, and a full disk is an outage
    /// caused by the thing that reports outages.
    struct Retention
    {
        int maxAgeDays{14};
        qint64 maxBytes{512LL * 1024 * 1024};
        int sweepMinutes{10};
    };

    MonitorService(EventStore *store, OperatorStore *operators, Retention retention,
                   QObject *parent = nullptr);

    /// One batch from one entity, with the entity name the transport verified.
    Q_INVOKABLE void take(const QVariantList &events, const QString &from);

    /// An entity saying it is still there. Silence is the case a monitor exists to notice,
    /// and it is indistinguishable from health without this.
    Q_INVOKABLE void heartbeat(const QString &entity, double sentMs);

    /// The console's question, answered as rows in the shape its model declares.
    Q_INVOKABLE QVariantList ask(const QString &text, const QString &entity,
                                 const QString &minimumSeverity, int limit) const;

    /// Everything carrying one trace id. One click's whole story, across every entity.
    Q_INVOKABLE QVariantList follow(const QString &traceId) const;

    /// One row per entity heard from, for the health strip.
    Q_INVOKABLE QVariantList entityRows() const;

    /// Is this an operator? The console's gate, and the only thing that opens it.
    Q_INVOKABLE bool signIn(const QString &name, const QString &password) const;

    double received() const;
    double stored() const;
    double dropped() const;

Q_SIGNALS:
    /// A batch landed. Every console republishes what it is showing.
    void arrived();
    void countersChanged();

private:
    struct Reporter
    {
        qint64 lastSeenMs{0};
        qint64 events{0};
        qint64 refusals{0};
    };

    EventStore *m_store{nullptr};
    OperatorStore *m_operators{nullptr};
    Retention m_retention;
    QTimer *m_sweep{nullptr};
    QHash<QString, Reporter> m_reporters;
    qint64 m_received{0};
    qint64 m_stored{0};
    qint64 m_dropped{0};
};

} // namespace SynQt

#endif // SYNQT_MONITORSERVICE_H
