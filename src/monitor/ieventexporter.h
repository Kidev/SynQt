// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IEVENTEXPORTER_H
#define SYNQT_IEVENTEXPORTER_H

#include "traceevent.h"

#include <QList>
#include <QString>

namespace SynQt {

/// The cold tier: somewhere the events also go, such as an OpenTelemetry collector,
/// Grafana, Loki or a hosted backend, alongside SynQt's own store.
///
/// Implementations are held to two rules:
///
/// - `take` returns immediately. A slow, unreachable or missing destination must not reach
///   the monitor's store, its console, or the entities being watched (which is why the
///   exporters live here and not in `Tracer`).
/// - Nothing queues without a bound. An exporter drops and counts what it dropped rather than
///   growing.
class IEventExporter
{
public:
    virtual ~IEventExporter();

    /// What this exporter is, for the record it writes about itself.
    virtual QString name() const = 0;

    /// One batch, on its way. Returns immediately, whatever the other end is doing.
    virtual void take(const QList<TraceEvent> &events) = 0;

    /// How many events left here, and how many were given up on because the other end was
    /// not keeping up. An operator reading a quiet dashboard has to be able to tell it
    /// from a hole.
    virtual qint64 exported() const = 0;
    virtual qint64 dropped() const = 0;
};

} // namespace SynQt

#endif // SYNQT_IEVENTEXPORTER_H
