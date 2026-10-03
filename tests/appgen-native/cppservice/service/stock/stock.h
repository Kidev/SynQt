// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <SynQtService>

// The whole of the stock entity. There is no Stock.qml.
class Stock : public SynQt::Service
{
    Q_OBJECT

public:
    explicit Stock(QObject *parent = nullptr);

protected:
    void componentComplete() override;
    void restock(SynQt::Caller *caller, const QString &sku, int count) override;
    int total(SynQt::Caller *caller) override;

private:
    int m_total{0};
};
