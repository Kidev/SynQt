// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Remote pages acceptance, edge side. The page store owns bytes and hashes; fetchPage's
// authorization is what actually keeps a scoped page off an under-scoped session.

#include "rep_pages_source.h"

#include <QTest>

class tst_PageStore : public QObject
{
    Q_OBJECT

private slots:
    void contractLowersToAUsablePod();
};

void tst_PageStore::contractLowersToAUsablePod()
{
    PageResponse response{};
    response.setStatus(QStringLiteral("ok"));
    response.setQml(QStringLiteral("import QtQuick\nItem {}"));
    response.setHash(QStringLiteral("abc"));
    response.setSeed(QStringLiteral("{}"));
    QCOMPARE(response.status(), QStringLiteral("ok"));
    QCOMPARE(response.hash(), QStringLiteral("abc"));
}

QTEST_MAIN(tst_PageStore)
#include "tst_pagestore.moc"
