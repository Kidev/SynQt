// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IEVENTEXPORTER_H
#define SYNQT_IEVENTEXPORTER_H

#include "traceevent.h"

#include <QList>
#include <QString>

namespace SynQt {

/// The cold tier. Somewhere the events also go, for an operator who already runs
/// something.
///
/// SynQt's own store is the answer to "where do the events go" for a deployment that wants
/// no second thing to operate. It is deliberately not the only answer: a team already
/// running an OpenTelemetry collector, Grafana, Loki or a hosted backend should be able to
/// point SynQt at it and keep the dashboards they have. That is all an exporter is.
///
/// Two rules the implementations are held to, and they are the reason this is an interface
/// rather than a call in `MonitorService`:
///
/// - `take` returns immediately. Whatever is at the other end may be slow, unreachable, or
///   gone. None of that may reach the monitor's own store or its console, and none of it
///   may reach the entities being watched (which is why the exporters live here and not in
///   `Tracer`).
/// - Nothing queues without a bound. An exporter in front of a collector that stopped
///   answering is the classic way a monitoring tool takes the machine down with it, so an
///   exporter drops and counts what it dropped rather than growing.
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
