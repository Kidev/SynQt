// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "spikesource.h"

#include <QDebug>
#include <QStandardItem>
#include <QTimer>

SpikeSource::SpikeSource(QObject *parent)
    : SpikeSourceSimpleSource{parent}
{
    for (int row{0}; row < 3; ++row) {
        m_model.appendRow(new QStandardItem{QStringLiteral("row %1").arg(row)});
    }
    setRows(&m_model);

    QTimer *timer{new QTimer{this}};
    connect(timer, &QTimer::timeout, this, &SpikeSource::tick);
    timer->start(1000);
}

QString SpikeSource::echo(const QString &message)
{
    // The one client->edge path in the spike. Its reply is the only direction that has ever
    // failed here (firefox-on-Linux CI, reply=false, persistently, while the edge->client prop,
    // signal, and model pushes all keep flowing). The client cannot tell "my invoke never
    // reached the edge" from "the reply never came back", so the edge says so itself: the verify
    // harness prints the edge's stdout as "[edge] ...", and the browser cases run one at a time,
    // so an invocation logged inside the failing case's window means the uplink works and the
    // reply is what is lost, and none logged means the invoke never arrived at all.
    qInfo().noquote() << QStringLiteral("M0 EDGE echo invoked message=%1").arg(message);
    return QStringLiteral("echo:") + message;
}

void SpikeSource::tick()
{
    ++m_tick;
    setCounter(counter() + 1);
    emit pinged(QStringLiteral("ping-%1").arg(m_tick));
    if ((m_tick % 3) == 0) {
        m_model.appendRow(new QStandardItem{QStringLiteral("row %1").arg(m_model.rowCount())});
    }
}
