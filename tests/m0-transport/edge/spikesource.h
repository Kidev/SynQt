// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_M0_SPIKESOURCE_H
#define SYNQT_M0_SPIKESOURCE_H

#include "rep_spike_source.h"

#include <QStandardItemModel>

// The authoritative Source for the M0 spike. It drives every QtRO direction from
// the edge so the client can prove all of them: it pushes the counter PROP, emits
// the pinged SIGNAL, mutates the rows MODEL on a timer, and answers the echo SLOT
// with a return value.
class SpikeSource : public SpikeSourceSimpleSource
{
    Q_OBJECT

public:
    explicit SpikeSource(QObject *parent = nullptr);

    QString echo(const QString &message) override;

private:
    void tick();

    QStandardItemModel m_model;
    int m_tick{0};
};

#endif // SYNQT_M0_SPIKESOURCE_H
