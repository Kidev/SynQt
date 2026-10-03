// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <SynQtService>

// Counts what stock says in C++. Lobby.qml, rooted at this class, does the rest.
class Lobby : public SynQt::Service
{
    Q_OBJECT

public:
    explicit Lobby(QObject *parent = nullptr);

protected:
    void componentComplete() override;
    int stockedCount(SynQt::Caller *caller) override;
};
