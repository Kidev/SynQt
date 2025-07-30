// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Remote pages acceptance, edge side. The page store owns bytes and hashes; fetchPage's
// authorization is what actually keeps a scoped page off an under-scoped session.

#include "rep_pages_source.h"
#include "pagestore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class tst_PageStore : public QObject
{
    Q_OBJECT

private slots:
    void contractLowersToAUsablePod();
    void storeHashesEachPageOnce();
    void storeRefusesAnUndeclaredRoute();
    void storeEmitsWhenAWatchedPageChanges();
    void storeRouteTableCarriesPathsAndScopes();
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

namespace {

QString writePage(const QDir &dir, const QString &name, const QByteArray &body)
{
    QFile file{dir.filePath(name)};
    if (!file.open(QIODevice::WriteOnly)) {
        return QString{};
    }
    file.write(body);
    file.close();
    return name;
}

} // namespace

void tst_PageStore::storeHashesEachPageOnce()
{
    QTemporaryDir pages;
    QVERIFY(pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("Campaign.qml"),
              "import QtQuick\nItem { objectName: \"campaign\" }");

    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/c/:campaign"), QStringLiteral("Campaign.qml"),
                  QString{});

    QVERIFY(store.hasRoute(QStringLiteral("/c/:campaign")));
    const QString hash{store.hashFor(QStringLiteral("/c/:campaign"))};
    QVERIFY(!hash.isEmpty());
    QCOMPARE(store.hashFor(QStringLiteral("/c/:campaign")), hash);
    QVERIFY(store.sourceFor(QStringLiteral("/c/:campaign")).contains(
        QStringLiteral("campaign")));
}

void tst_PageStore::storeRefusesAnUndeclaredRoute()
{
    QTemporaryDir pages;
    SynQt::PageStore store{pages.path()};
    QVERIFY(!store.hasRoute(QStringLiteral("/nope")));
    QVERIFY(store.sourceFor(QStringLiteral("/nope")).isEmpty());
    QVERIFY(store.hashFor(QStringLiteral("/nope")).isEmpty());
    // Path traversal is not a lookup that can succeed: routes are matched
    // exactly, never concatenated onto the pages directory.
    QVERIFY(store.sourceFor(QStringLiteral("../../etc/passwd")).isEmpty());
}

void tst_PageStore::storeEmitsWhenAWatchedPageChanges()
{
    QTemporaryDir pages;
    const QDir dir{pages.path()};
    writePage(dir, QStringLiteral("Campaign.qml"), "import QtQuick\nItem {}");

    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/c"), QStringLiteral("Campaign.qml"), QString{});
    const QString before{store.hashFor(QStringLiteral("/c"))};
    store.setWatching(true);

    QSignalSpy changed{&store, &SynQt::PageStore::pageChanged};
    writePage(dir, QStringLiteral("Campaign.qml"),
              "import QtQuick\nItem { objectName: \"edited\" }");
    QVERIFY(changed.wait(5000));
    QCOMPARE(changed.at(0).at(0).toString(), QStringLiteral("/c"));
    QVERIFY(store.hashFor(QStringLiteral("/c")) != before);
}

void tst_PageStore::storeRouteTableCarriesPathsAndScopes()
{
    QTemporaryDir pages;
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"), "import QtQuick\nItem {}");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));

    // Not brace-init: QJsonArray has an initializer_list<QJsonValue>
    // constructor, and a braced single argument of a convertible type binds
    // to that overload instead of copy-constructing, wrapping the whole
    // array as one nested element.
    const QJsonArray table =
        QJsonDocument::fromJson(store.routeTableJson().toUtf8()).array();
    QCOMPARE(table.size(), 1);
    QCOMPARE(table.at(0).toObject().value(QStringLiteral("path")).toString(),
             QStringLiteral("/admin/rules"));
    QCOMPARE(table.at(0).toObject().value(QStringLiteral("scope")).toString(),
             QStringLiteral("staff"));
}

QTEST_MAIN(tst_PageStore)
#include "tst_pagestore.moc"
