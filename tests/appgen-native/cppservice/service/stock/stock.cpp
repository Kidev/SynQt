// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "stock.h"

#include <QtConcurrent/QtConcurrentMap>

#include <QList>
#include <QVariantList>
#include <QVariantMap>

Stock::Stock(QObject *parent)
    : SynQt::Service{parent}
{
}

void Stock::componentComplete()
{
    setHeadline(QStringLiteral("stocked in C++"));
    qInfo("SYNQT-CPP stock has its log: %s", log() != nullptr ? "yes" : "no");
}

void Stock::restock(SynQt::Caller *caller, const QString &sku, int count)
{
    // Only an entity restocks, and the bound on `sku` has already been checked.
    if (caller == nullptr || !caller->isEntity()) {
        return;
    }
    // The work that wanted C++ and a library: summed on the thread pool.
    // Parenthesized: braces would build the two-element list {count, 1}.
    const QList<int> units(count, 1);
    const int added{QtConcurrent::blockingMappedReduced(
        units, [](int unit) { return unit; },
        [](int &sum, int unit) { sum += unit; })};
    m_total += added;
    // Copy-initialized: braces would wrap the rows in one more list.
    QVariantList rows = shelfRows();
    rows.append(QVariantMap{{QStringLiteral("sku"), sku}, {QStringLiteral("count"), added}});
    setShelf(rows);
    qInfo("SYNQT-CPP stock restock %s x%d for %s", qUtf8Printable(sku), added,
          qUtf8Printable(caller->entity()));
    emit restocked(sku, added);
}

int Stock::total(SynQt::Caller *caller)
{
    Q_UNUSED(caller)
    return m_total;
}
