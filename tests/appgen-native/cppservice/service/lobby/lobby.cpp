// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "lobby.h"

Lobby::Lobby(QObject *parent)
    : SynQt::Service{parent}
{
}

void Lobby::componentComplete()
{
    StockConsumer *consumed{stock()};
    qInfo("SYNQT-CPP lobby sees stock: %s", consumed != nullptr ? "yes" : "no");
    if (consumed == nullptr) {
        return;
    }
    // Published, so a caller can wait until this entity hears stock before restocking it.
    setWatching(consumed->isReady());
    connect(consumed, &StockConsumer::readyChanged, this, [this, consumed]() {
        setWatching(consumed->isReady());
    });
    connect(consumed, &StockConsumer::restocked, this, [this](const QString &, int count) {
        setStocked(stocked() + count);
    });
}

int Lobby::stockedCount(SynQt::Caller *caller)
{
    Q_UNUSED(caller)
    return stocked();
}
