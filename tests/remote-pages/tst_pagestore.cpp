// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Remote pages acceptance, edge side. The page store owns bytes and hashes; fetchPage's
// authorization is what actually keeps a scoped page off an under-scoped session.

#include "rep_pages_source.h"
#include "pagesservice.h"
#include "pagestore.h"
#include "caller.h"

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
    void storeWatchSurvivesAnAtomicReplace();
    void fetchRefusesAnUnderScopedCaller();
    void fetchServesAnAuthorizedCaller();
    void fetchReportsNotModifiedForAMatchingHash();
    void fetchRefusesAnUndeclaredRoute();
    void fetchSeedsThePageWhenAProviderIsSet();
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

// Replace name with new content under a fresh inode: write a sibling temp
// file, then rename it over the watched path. Unlike writePage's in-place
// truncate (same inode), this is what a real editor's atomic save does, and
// it is what drops a naive QFileSystemWatcher watch.
void replacePage(const QDir &dir, const QString &name, const QByteArray &body)
{
    const QString temporaryName{name + QStringLiteral(".tmp")};
    writePage(dir, temporaryName, body);
    QFile::remove(dir.filePath(name));
    QFile::rename(dir.filePath(temporaryName), dir.filePath(name));
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

void tst_PageStore::storeWatchSurvivesAnAtomicReplace()
{
    QTemporaryDir pages;
    const QDir dir{pages.path()};
    writePage(dir, QStringLiteral("Campaign.qml"), "import QtQuick\nItem {}");

    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/c"), QStringLiteral("Campaign.qml"), QString{});
    store.setWatching(true);

    QSignalSpy changed{&store, &SynQt::PageStore::pageChanged};

    replacePage(dir, QStringLiteral("Campaign.qml"),
                "import QtQuick\nItem { objectName: \"first\" }");
    QVERIFY(changed.wait(5000));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toString(), QStringLiteral("/c"));
    const QString afterFirst{store.hashFor(QStringLiteral("/c"))};
    QVERIFY(!afterFirst.isEmpty());

    // The watch must have survived the first replace: a second replace still
    // has to be observed, proving onFileChanged re-armed the watcher instead
    // of silently losing it.
    replacePage(dir, QStringLiteral("Campaign.qml"),
                "import QtQuick\nItem { objectName: \"second\" }");
    QVERIFY(changed.wait(5000));
    QCOMPARE(changed.count(), 2);
    QCOMPARE(changed.at(1).at(0).toString(), QStringLiteral("/c"));
    QVERIFY(store.hashFor(QStringLiteral("/c")) != afterFirst);
}

void tst_PageStore::fetchRefusesAnUnderScopedCaller()
{
    QTemporaryDir pages;
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"),
              "import QtQuick\nItem { objectName: \"secretRules\" }");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));
    SynQt::PagesService service{&store};

    SynQt::Caller anonymous;
    anonymous.setScope(QStringLiteral("anonymous"));
    const PageResponse response{service.fetchPageFor(
        QStringLiteral("/admin/rules"), QString{}, &anonymous)};

    QCOMPARE(response.status(), QStringLiteral("forbidden"));
    // The confidentiality guarantee is that nothing of the page comes back, not merely
    // that a flag says no.
    QVERIFY(response.qml().isEmpty());
    QVERIFY(!response.qml().contains(QStringLiteral("secretRules")));
    QVERIFY(response.hash().isEmpty());
}

void tst_PageStore::fetchServesAnAuthorizedCaller()
{
    QTemporaryDir pages;
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"),
              "import QtQuick\nItem { objectName: \"secretRules\" }");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));
    SynQt::PagesService service{&store};

    SynQt::Caller staff;
    staff.setScope(QStringLiteral("staff"));
    const PageResponse response{service.fetchPageFor(
        QStringLiteral("/admin/rules"), QString{}, &staff)};

    QCOMPARE(response.status(), QStringLiteral("ok"));
    QVERIFY(response.qml().contains(QStringLiteral("secretRules")));
    QCOMPARE(response.hash(), store.hashFor(QStringLiteral("/admin/rules")));
}

void tst_PageStore::fetchReportsNotModifiedForAMatchingHash()
{
    QTemporaryDir pages;
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/c"), QStringLiteral("C.qml"), QString{});
    SynQt::PagesService service{&store};

    SynQt::Caller caller;
    const QString hash{store.hashFor(QStringLiteral("/c"))};
    const PageResponse response{
        service.fetchPageFor(QStringLiteral("/c"), hash, &caller)};

    QCOMPARE(response.status(), QStringLiteral("notModified"));
    QVERIFY(response.qml().isEmpty());
}

void tst_PageStore::fetchRefusesAnUndeclaredRoute()
{
    QTemporaryDir pages;
    SynQt::PageStore store{pages.path()};
    SynQt::PagesService service{&store};
    SynQt::Caller caller;
    const PageResponse response{
        service.fetchPageFor(QStringLiteral("/anything"), QString{}, &caller)};
    QCOMPARE(response.status(), QStringLiteral("notFound"));
    QVERIFY(response.qml().isEmpty());
}

void tst_PageStore::fetchSeedsThePageWhenAProviderIsSet()
{
    QTemporaryDir pages;
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/c/:campaign"), QStringLiteral("C.qml"), QString{});
    SynQt::PagesService service{&store};
    service.setSeedProvider([](const QString &route, const QVariantMap &parameters,
                              SynQt::Caller *caller) {
        Q_UNUSED(route);
        Q_UNUSED(caller);
        return QStringLiteral("{\"campaign\":\"%1\"}")
            .arg(parameters.value(QStringLiteral("campaign")).toString());
    });

    SynQt::Caller caller;
    const PageResponse response{service.fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, &caller)};
    QCOMPARE(response.status(), QStringLiteral("ok"));
    QCOMPARE(response.seed(), QStringLiteral("{\"campaign\":\"summer\"}"));
}

QTEST_MAIN(tst_PageStore)
#include "tst_pagestore.moc"
