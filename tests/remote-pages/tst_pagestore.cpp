// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Remote pages acceptance, edge side. The page store owns bytes and hashes; fetchPage's
// authorization is what actually keeps a scoped page off an under-scoped session.

#include "rep_pages_source.h"
#include "pagesservice.h"
#include "pagesedgesource.h"
#include "pagestore.h"
#include "caller.h"
#include "sessionmanager.h"
#include "webedge.h"
#include "webedgeconfig.h"
#include "websockettransport.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QWebSocket>

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
    void fetchSeedsANotModifiedReplyForTheRequestedParameters();
    void fetchRefusalsCarryNoSeed();
    void fetchPrefersTheMoreLiteralRoute();

    // Task 6b: the Pages connect point actually hosted by WebEdge. These construct
    // the real hosted Source (PagesEdgeSource) the way WebEdge::hostConnection()
    // does -- a live session-bound Caller via Caller::forUser, then the Source
    // built over it -- instead of calling PagesService directly, which is what
    // every test above already covers.
    void hostedFetchRefusesAnUnderScopedCaller();
    void hostedFetchServesAnAuthorizedCaller();
    void hostedRouteTablePublishesPathsAndScopes();
    void hostedPageChangedRelaysToTheSource();
    void edgeWithNoPagesHostsNoPagesConnectPoint();
    void edgeWithPagesHostsAReachablePagesConnectPoint();

    // Task 7b: the app-facing seed hook, built by WebEdge from each page's `seed:` and
    // dispatched through the one provider installed on the shared PagesService.
    void edgeSeedsAPageFromItsHook();
    void edgeSeedHookReadsTheCaller();
    void edgePageWithoutAHookHasAnEmptySeed();
    void edgeDegradesAHookThatFailsToLoad();
    void edgeDegradesAHookWithNoSeedFunction();
    void edgeLogsAHookWithNoSeedFunctionOnlyOnce();
    void edgeRefusesASeedThatIsNotAnObject();
    void edgeRefusesASeedNestedTooDeeply();
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

// A Caller built the way a real accepted connection gets one: a live session
// (SynQt::SessionManager::createSession()) bound to a Caller via
// SynQt::Caller::forUser(), exactly as WebEdge::hostConnection() does. Also
// installs the scope order the same way WebEdge::hostConnection() does
// (webedge.cpp:732), so fetchPageFor()'s hasScope() checks rank scopes
// hierarchically here too, rather than only ever exercising the
// exact-string-equality branch. Test-only: a bare, unbound Caller has no
// session and cannot authorize anything for real, so this composes only the
// existing production API rather than widening Caller itself for testing.
SynQt::Caller *scopedCaller(SynQt::SessionManager &sessions, const QString &scope,
                            QObject *parent)
{
    const QByteArray sessionId{sessions.createSession(scope)};
    SynQt::Caller *caller{
        SynQt::Caller::forUser(QString{}, &sessions, sessionId, nullptr, parent)};
    static const QStringList order{QStringLiteral("anonymous"), QStringLiteral("staff")};
    caller->setScopeOrder(order, true);
    return caller;
}

// A plaintext (dev-style) edge config with no pages: the negative half of
// "no pages configured hosts nothing".
SynQt::WebEdgeConfig edgeConfigWithNoPages(const QString &bundleDir)
{
    SynQt::WebEdgeConfig config{};
    config.bundleDir = bundleDir;
    config.host = QStringLiteral("127.0.0.1");
    config.port = 0;  // ephemeral, plaintext (no certFile/keyFile): a dev-style edge
    return config;
}

// The same, plus one declared page, for the positive half.
SynQt::WebEdgeConfig edgeConfigWithOnePage(const QString &bundleDir, const QString &pagesDir)
{
    SynQt::WebEdgeConfig config{edgeConfigWithNoPages(bundleDir)};
    config.pagesDir = pagesDir;
    SynQt::WebEdgePage page{};
    page.path = QStringLiteral("/c");
    page.file = QStringLiteral("C.qml");
    page.scope = QString{};
    config.pages.append(page);
    return config;
}

// An edge serving one parameterized page whose seed hook is hookFile; an empty hookFile
// is the common case of a route that declares no seed at all.
SynQt::WebEdgeConfig edgeConfigWithASeededPage(const QString &bundleDir,
                                               const QString &pagesDir,
                                               const QString &hookFile,
                                               const QString &scope = QString{})
{
    SynQt::WebEdgeConfig config{edgeConfigWithNoPages(bundleDir)};
    config.pagesDir = pagesDir;
    config.scopeOrder = QStringList{QStringLiteral("anonymous"), QStringLiteral("staff")};
    SynQt::WebEdgePage page{};
    page.path = QStringLiteral("/c/:campaign");
    page.file = QStringLiteral("C.qml");
    page.scope = scope;
    page.seed = hookFile;
    config.pages.append(page);
    return config;
}

// A Caller bound to a live session on the edge's own session store, the way
// WebEdge::hostConnection() binds one per accepted connection.
SynQt::Caller *edgeCaller(SynQt::WebEdge &edge, const QString &scope, QObject *parent)
{
    const QByteArray token{edge.sessionManager()->createSession(scope)};
    SynQt::Caller *caller{SynQt::Caller::forUser(
        QStringLiteral("Pages"), edge.sessionManager(), token, nullptr, parent)};
    static const QStringList order{QStringLiteral("anonymous"), QStringLiteral("staff")};
    caller->setScopeOrder(order, true);
    return caller;
}

QJsonObject seedObject(const PageResponse &response)
{
    return QJsonDocument::fromJson(response.seed().toUtf8()).object();
}

// Counts every message Qt emits while it is in scope, whoever emits it: the framework's
// own "No such method" complaint counts the same as one of ours. A misbehaving hook is
// reachable from the browser, so what matters is not which line logs but that asking
// again does not log again.
class MessageCounter
{
public:
    MessageCounter()
    {
        s_count = 0;
        s_previous = qInstallMessageHandler(&MessageCounter::handle);
    }

    ~MessageCounter()
    {
        qInstallMessageHandler(s_previous);
    }

    int count() const { return s_count; }

private:
    static void handle(QtMsgType type, const QMessageLogContext &context,
                       const QString &message)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        Q_UNUSED(message);
        ++s_count;
    }

    static inline int s_count{0};
    static inline QtMessageHandler s_previous{nullptr};
};

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
    QTemporaryDir pages{};
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"),
              "import QtQuick\nItem { objectName: \"secretRules\" }");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));
    SynQt::PagesService service{&store};

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    SynQt::Caller *anonymous{scopedCaller(sessions, QStringLiteral("anonymous"), this)};
    const PageResponse response{service.fetchPageFor(
        QStringLiteral("/admin/rules"), QString{}, anonymous)};

    QCOMPARE(response.status(), QStringLiteral("forbidden"));
    // The confidentiality guarantee is that nothing of the page comes back, not merely
    // that a flag says no.
    QVERIFY(response.qml().isEmpty());
    QVERIFY(!response.qml().contains(QStringLiteral("secretRules")));
    QVERIFY(response.hash().isEmpty());
}

void tst_PageStore::fetchServesAnAuthorizedCaller()
{
    QTemporaryDir pages{};
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"),
              "import QtQuick\nItem { objectName: \"secretRules\" }");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));
    SynQt::PagesService service{&store};

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    SynQt::Caller *staff{scopedCaller(sessions, QStringLiteral("staff"), this)};
    const PageResponse response{service.fetchPageFor(
        QStringLiteral("/admin/rules"), QString{}, staff)};

    QCOMPARE(response.status(), QStringLiteral("ok"));
    QVERIFY(response.qml().contains(QStringLiteral("secretRules")));
    QCOMPARE(response.hash(), store.hashFor(QStringLiteral("/admin/rules")));
}

void tst_PageStore::fetchReportsNotModifiedForAMatchingHash()
{
    QTemporaryDir pages{};
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/c"), QStringLiteral("C.qml"), QString{});
    SynQt::PagesService service{&store};

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    SynQt::Caller *caller{scopedCaller(sessions, QStringLiteral("anonymous"), this)};
    const QString hash{store.hashFor(QStringLiteral("/c"))};
    const PageResponse response{
        service.fetchPageFor(QStringLiteral("/c"), hash, caller)};

    QCOMPARE(response.status(), QStringLiteral("notModified"));
    QVERIFY(response.qml().isEmpty());
}

void tst_PageStore::fetchRefusesAnUndeclaredRoute()
{
    QTemporaryDir pages{};
    SynQt::PageStore store{pages.path()};
    SynQt::PagesService service{&store};
    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    SynQt::Caller *caller{scopedCaller(sessions, QStringLiteral("anonymous"), this)};
    const PageResponse response{
        service.fetchPageFor(QStringLiteral("/anything"), QString{}, caller)};
    QCOMPARE(response.status(), QStringLiteral("notFound"));
    QVERIFY(response.qml().isEmpty());
}

void tst_PageStore::fetchSeedsThePageWhenAProviderIsSet()
{
    QTemporaryDir pages{};
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

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    SynQt::Caller *caller{scopedCaller(sessions, QStringLiteral("anonymous"), this)};
    const PageResponse response{service.fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, caller)};
    QCOMPARE(response.status(), QStringLiteral("ok"));
    QCOMPARE(response.seed(), QStringLiteral("{\"campaign\":\"summer\"}"));
}

void tst_PageStore::fetchSeedsANotModifiedReplyForTheRequestedParameters()
{
    // A page's hash is the hash of the page FILE, so every parameterization of one
    // route shares it. A visitor who reads "/c/summer-sale" and then navigates to
    // "/c/black-friday" sends the first page's hash along with the second path: the
    // reply is notModified, and the client deliberately keeps its previous seed on an
    // empty one (router.cpp). A notModified carrying no seed would therefore paint
    // black-friday with the summer-sale seed. Only the bulky qml payload is worth
    // skipping on a cache hit; the seed is small and parameter-dependent, so it must
    // always describe the request that was actually made.
    QTemporaryDir pages{};
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

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    SynQt::Caller *caller{scopedCaller(sessions, QStringLiteral("anonymous"), this)};

    const PageResponse first{service.fetchPageFor(
        QStringLiteral("/c/summer-sale"), QString{}, caller)};
    QCOMPARE(first.status(), QStringLiteral("ok"));
    QCOMPARE(first.seed(), QStringLiteral("{\"campaign\":\"summer-sale\"}"));

    const PageResponse second{service.fetchPageFor(
        QStringLiteral("/c/black-friday"), first.hash(), caller)};
    QCOMPARE(second.status(), QStringLiteral("notModified"));
    QVERIFY(second.qml().isEmpty());
    QCOMPARE(second.hash(), first.hash());
    QCOMPARE(second.seed(), QStringLiteral("{\"campaign\":\"black-friday\"}"));
}

void tst_PageStore::fetchRefusalsCarryNoSeed()
{
    // Producing the seed on the notModified path must not have loosened what a refusal
    // carries: forbidden and notFound still come back with nothing at all, seed
    // included, and the provider is never even asked (it runs only after the scope
    // check, so it may read privileged state).
    QTemporaryDir pages{};
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"),
              "import QtQuick\nItem { objectName: \"secretRules\" }");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));
    SynQt::PagesService service{&store};
    bool asked{false};
    service.setSeedProvider([&asked](const QString &route, const QVariantMap &parameters,
                                     SynQt::Caller *caller) {
        Q_UNUSED(route);
        Q_UNUSED(parameters);
        Q_UNUSED(caller);
        asked = true;
        return QStringLiteral("{\"secret\":\"leaked\"}");
    });

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    SynQt::Caller *anonymous{scopedCaller(sessions, QStringLiteral("anonymous"), this)};

    const PageResponse forbidden{service.fetchPageFor(
        QStringLiteral("/admin/rules"), QString{}, anonymous)};
    QCOMPARE(forbidden.status(), QStringLiteral("forbidden"));
    QVERIFY(forbidden.qml().isEmpty());
    QVERIFY(forbidden.hash().isEmpty());
    QVERIFY(forbidden.seed().isEmpty());

    // Even the hash of the page it already holds must not turn a refusal into a reply.
    const PageResponse cached{service.fetchPageFor(
        QStringLiteral("/admin/rules"), store.hashFor(QStringLiteral("/admin/rules")),
        anonymous)};
    QCOMPARE(cached.status(), QStringLiteral("forbidden"));
    QVERIFY(cached.hash().isEmpty());
    QVERIFY(cached.seed().isEmpty());

    const PageResponse missing{service.fetchPageFor(
        QStringLiteral("/nowhere"), QString{}, anonymous)};
    QCOMPARE(missing.status(), QStringLiteral("notFound"));
    QVERIFY(missing.qml().isEmpty());
    QVERIFY(missing.hash().isEmpty());
    QVERIFY(missing.seed().isEmpty());

    QVERIFY2(!asked, "the seed provider must run only after the scope check passes");
}

void tst_PageStore::fetchPrefersTheMoreLiteralRoute()
{
    QTemporaryDir pages{};
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"),
              "import QtQuick\nItem { objectName: \"literalRules\" }");
    writePage(QDir{pages.path()}, QStringLiteral("Page.qml"),
              "import QtQuick\nItem { objectName: \"parameterPage\" }");
    SynQt::PageStore store{pages.path()};
    // Precedence must come from RoutePattern::literalSegmentCount()
    // (routepattern.h), never from PageStore::declaredRoutes()'s QHash order
    // (unspecified, unstable) or from declaration order, which is why both
    // are declared here regardless of which happens to be added first.
    store.addPage(QStringLiteral("/admin/:page"), QStringLiteral("Page.qml"), QString{});
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));
    SynQt::PagesService service{&store};

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    SynQt::Caller *staff{scopedCaller(sessions, QStringLiteral("staff"), this)};
    const PageResponse response{service.fetchPageFor(
        QStringLiteral("/admin/rules"), QString{}, staff)};

    // The literal route wins: its file (and its staff scope) is what actually
    // answered, not the parameterized route that also matches "/admin/rules".
    QCOMPARE(response.status(), QStringLiteral("ok"));
    QVERIFY(response.qml().contains(QStringLiteral("literalRules")));
    QCOMPARE(response.hash(), store.hashFor(QStringLiteral("/admin/rules")));
}

void tst_PageStore::hostedFetchRefusesAnUnderScopedCaller()
{
    QTemporaryDir pages{};
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"),
              "import QtQuick\nItem { objectName: \"secretRules\" }");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));
    SynQt::PagesService service{&store};

    // Built exactly the way WebEdge::hostConnection() builds a per-connection
    // Caller and Source (webedge.cpp): a live session bound through
    // Caller::forUser, handed to a fresh PagesEdgeSource. The forbidden guarantee
    // must hold through THIS object, the one WebEdge actually hosts, not through
    // PagesService called directly (the tests above already prove that half).
    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    const QByteArray token{sessions.createSession(QStringLiteral("anonymous"))};
    SynQt::Caller *caller{SynQt::Caller::forUser(
        QStringLiteral("Pages"), &sessions, token, nullptr, this)};
    static const QStringList order{QStringLiteral("anonymous"), QStringLiteral("staff")};
    caller->setScopeOrder(order, true);

    SynQt::PagesEdgeSource source{&store, &service, caller, this};
    caller->setSource(&source);

    const PageResponse response{
        source.fetchPage(QStringLiteral("/admin/rules"), QString{})};
    QCOMPARE(response.status(), QStringLiteral("forbidden"));
    QVERIFY(response.qml().isEmpty());
    QVERIFY(!response.qml().contains(QStringLiteral("secretRules")));
    QVERIFY(response.hash().isEmpty());
}

void tst_PageStore::hostedFetchServesAnAuthorizedCaller()
{
    QTemporaryDir pages{};
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"),
              "import QtQuick\nItem { objectName: \"secretRules\" }");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));
    SynQt::PagesService service{&store};

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    const QByteArray token{sessions.createSession(QStringLiteral("staff"))};
    SynQt::Caller *caller{SynQt::Caller::forUser(
        QStringLiteral("Pages"), &sessions, token, nullptr, this)};
    static const QStringList order{QStringLiteral("anonymous"), QStringLiteral("staff")};
    caller->setScopeOrder(order, true);

    SynQt::PagesEdgeSource source{&store, &service, caller, this};
    caller->setSource(&source);

    const PageResponse response{
        source.fetchPage(QStringLiteral("/admin/rules"), QString{})};
    QCOMPARE(response.status(), QStringLiteral("ok"));
    QVERIFY(response.qml().contains(QStringLiteral("secretRules")));
    QCOMPARE(response.hash(), store.hashFor(QStringLiteral("/admin/rules")));
}

void tst_PageStore::hostedRouteTablePublishesPathsAndScopes()
{
    QTemporaryDir pages{};
    writePage(QDir{pages.path()}, QStringLiteral("Rules.qml"), "import QtQuick\nItem {}");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/admin/rules"), QStringLiteral("Rules.qml"),
                  QStringLiteral("staff"));
    SynQt::PagesService service{&store};

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    const QByteArray token{sessions.createSession()};
    SynQt::Caller *caller{SynQt::Caller::forUser(
        QStringLiteral("Pages"), &sessions, token, nullptr, this)};

    SynQt::PagesEdgeSource source{&store, &service, caller, this};
    caller->setSource(&source);

    // Not brace-init: QJsonArray has an initializer_list<QJsonValue> constructor
    // and would wrap a single convertible argument instead of copying it.
    const QJsonArray table =
        QJsonDocument::fromJson(source.routeTable().toUtf8()).array();
    QCOMPARE(table.size(), 1);
    QCOMPARE(table.at(0).toObject().value(QStringLiteral("path")).toString(),
             QStringLiteral("/admin/rules"));
    QCOMPARE(table.at(0).toObject().value(QStringLiteral("scope")).toString(),
             QStringLiteral("staff"));
}

void tst_PageStore::hostedPageChangedRelaysToTheSource()
{
    QTemporaryDir pages{};
    const QDir dir{pages.path()};
    writePage(dir, QStringLiteral("C.qml"), "import QtQuick\nItem {}");
    SynQt::PageStore store{pages.path()};
    store.addPage(QStringLiteral("/c"), QStringLiteral("C.qml"), QString{});
    store.setWatching(true);
    SynQt::PagesService service{&store};

    SynQt::SessionManager sessions{QStringLiteral("anonymous"), 0};
    const QByteArray token{sessions.createSession()};
    SynQt::Caller *caller{SynQt::Caller::forUser(
        QStringLiteral("Pages"), &sessions, token, nullptr, this)};

    SynQt::PagesEdgeSource source{&store, &service, caller, this};
    caller->setSource(&source);

    QSignalSpy changed{&source, &SynQt::PagesEdgeSource::pageChanged};
    writePage(dir, QStringLiteral("C.qml"),
              "import QtQuick\nItem { objectName: \"edited\" }");
    QVERIFY(changed.wait(5000));
    QCOMPARE(changed.at(0).at(0).toString(), QStringLiteral("/c"));
    QCOMPARE(changed.at(0).at(1).toString(), store.hashFor(QStringLiteral("/c")));
}

void tst_PageStore::edgeWithNoPagesHostsNoPagesConnectPoint()
{
    QTemporaryDir bundle{};
    QVERIFY(bundle.isValid());
    SynQt::WebEdgeConfig config{edgeConfigWithNoPages(bundle.path())};
    SynQt::WebEdge edge{config, nullptr};
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));

    const QByteArray token{edge.sessionManager()->createSession()};
    QWebSocket socket;
    SynQt::WebSocketTransport transport{&socket};
    QVERIFY(transport.open(QIODevice::ReadWrite));

    QRemoteObjectNode node;
    node.addClientSideConnection(&transport);

    QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
    request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
    request.setRawHeader("Cookie", QByteArrayLiteral("synqt_session=") + token);
    socket.open(request);

    QScopedPointer<QRemoteObjectDynamicReplica> replica{
        node.acquireDynamic(QStringLiteral("Pages"))};
    // An app that never configured pages must never expose the connect point:
    // waitForSource times out and returns false, rather than the connect point
    // eventually appearing.
    QVERIFY(!replica->waitForSource(2000));
}

void tst_PageStore::edgeWithPagesHostsAReachablePagesConnectPoint()
{
    QTemporaryDir bundle{};
    QVERIFY(bundle.isValid());
    QTemporaryDir pages{};
    QVERIFY(pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");

    SynQt::WebEdgeConfig config{edgeConfigWithOnePage(bundle.path(), pages.path())};
    SynQt::WebEdge edge{config, nullptr};
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));

    const QByteArray token{edge.sessionManager()->createSession()};
    QWebSocket socket;
    SynQt::WebSocketTransport transport{&socket};
    QVERIFY(transport.open(QIODevice::ReadWrite));

    QRemoteObjectNode node;
    node.addClientSideConnection(&transport);

    QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
    request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
    request.setRawHeader("Cookie", QByteArrayLiteral("synqt_session=") + token);
    socket.open(request);

    QScopedPointer<QRemoteObjectDynamicReplica> replica{
        node.acquireDynamic(QStringLiteral("Pages"))};
    QVERIFY2(replica->waitForSource(5000),
             "a project that configures pages must expose the Pages connect point");
}

void tst_PageStore::edgeSeedsAPageFromItsHook()
{
    QTemporaryDir bundle{};
    QTemporaryDir pages{};
    QVERIFY(bundle.isValid() && pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");

    QQmlEngine engine;
    SynQt::WebEdgeConfig config{edgeConfigWithASeededPage(
        bundle.path(), pages.path(),
        QStringLiteral(REMOTE_PAGES_SRCDIR "/seeds/Campaign.qml"))};
    SynQt::WebEdge edge{config, &engine};
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));
    QVERIFY(edge.pagesService() != nullptr);

    SynQt::Caller *caller{edgeCaller(edge, QStringLiteral("anonymous"), this)};
    const PageResponse response{edge.pagesService()->fetchPageFor(
        QStringLiteral("/c/summer-sale"), QString{}, caller)};

    QCOMPARE(response.status(), QStringLiteral("ok"));
    const QJsonObject seed{seedObject(response)};
    // The hook is handed the matched route pattern and the concrete parameters, which is
    // what makes one page file able to paint every parameterization of its route.
    QCOMPARE(seed.value(QStringLiteral("route")).toString(),
             QStringLiteral("/c/:campaign"));
    QCOMPARE(seed.value(QStringLiteral("headline")).toString(),
             QStringLiteral("summer-sale"));
}

void tst_PageStore::edgeSeedHookReadsTheCaller()
{
    QTemporaryDir bundle{};
    QTemporaryDir pages{};
    QVERIFY(bundle.isValid() && pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");

    QQmlEngine engine;
    SynQt::WebEdgeConfig config{edgeConfigWithASeededPage(
        bundle.path(), pages.path(),
        QStringLiteral(REMOTE_PAGES_SRCDIR "/seeds/Scoped.qml"))};
    SynQt::WebEdge edge{config, &engine};
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));

    // One provider is installed on the SHARED service, but each call carries its own
    // connection's Caller: two callers of different scope must get different seeds, or
    // the provider is capturing a Caller instead of reading the one it is handed.
    SynQt::Caller *staff{edgeCaller(edge, QStringLiteral("staff"), this)};
    SynQt::Caller *anonymous{edgeCaller(edge, QStringLiteral("anonymous"), this)};

    const PageResponse privileged{edge.pagesService()->fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, staff)};
    const PageResponse ordinary{edge.pagesService()->fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, anonymous)};

    QCOMPARE(seedObject(privileged).value(QStringLiteral("audience")).toString(),
             QStringLiteral("staff"));
    QCOMPARE(seedObject(privileged).value(QStringLiteral("margin")).toInt(), 42);
    QCOMPARE(seedObject(ordinary).value(QStringLiteral("audience")).toString(),
             QStringLiteral("public"));
    QVERIFY(!ordinary.seed().contains(QStringLiteral("margin")));

    // The staff seed must not have leaked into the next caller's answer either: ask
    // again, in the other order, and the two still disagree.
    const PageResponse again{edge.pagesService()->fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, staff)};
    QCOMPARE(seedObject(again).value(QStringLiteral("margin")).toInt(), 42);
}

void tst_PageStore::edgePageWithoutAHookHasAnEmptySeed()
{
    QTemporaryDir bundle{};
    QTemporaryDir pages{};
    QVERIFY(bundle.isValid() && pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");

    QQmlEngine engine;
    SynQt::WebEdgeConfig config{
        edgeConfigWithASeededPage(bundle.path(), pages.path(), QString{})};
    SynQt::WebEdge edge{config, &engine};
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));

    SynQt::Caller *caller{edgeCaller(edge, QStringLiteral("anonymous"), this)};
    const PageResponse response{edge.pagesService()->fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, caller)};

    // A project that does not use the feature pays nothing for it: no hook is built and
    // no seed is sent.
    QCOMPARE(response.status(), QStringLiteral("ok"));
    QVERIFY(response.seed().isEmpty());
    QVERIFY(!response.qml().isEmpty());
}

void tst_PageStore::edgeDegradesAHookThatFailsToLoad()
{
    QTemporaryDir bundle{};
    QTemporaryDir pages{};
    QVERIFY(bundle.isValid() && pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");

    QQmlEngine engine;
    SynQt::WebEdgeConfig config{edgeConfigWithASeededPage(
        bundle.path(), pages.path(),
        QStringLiteral(REMOTE_PAGES_SRCDIR "/seeds/Broken.qml"))};
    SynQt::WebEdge edge{config, &engine};
    // The hook reports itself once and is then simply absent; a broken hook must never
    // stop the edge from starting or the page from being delivered.
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression{QStringLiteral("page seed hook .* failed to load")});
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));

    SynQt::Caller *caller{edgeCaller(edge, QStringLiteral("anonymous"), this)};
    const PageResponse response{edge.pagesService()->fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, caller)};

    QCOMPARE(response.status(), QStringLiteral("ok"));
    QVERIFY(!response.qml().isEmpty());
    QVERIFY(response.seed().isEmpty());
}

void tst_PageStore::edgeDegradesAHookWithNoSeedFunction()
{
    QTemporaryDir bundle{};
    QTemporaryDir pages{};
    QVERIFY(bundle.isValid() && pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");

    QQmlEngine engine;
    SynQt::WebEdgeConfig config{edgeConfigWithASeededPage(
        bundle.path(), pages.path(),
        QStringLiteral(REMOTE_PAGES_SRCDIR "/seeds/NoFunction.qml"))};
    SynQt::WebEdge edge{config, &engine};
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression{QStringLiteral("declares no seedFor")});
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));

    SynQt::Caller *caller{edgeCaller(edge, QStringLiteral("anonymous"), this)};
    const PageResponse response{edge.pagesService()->fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, caller)};

    // The hook loaded but declares no seedFor: it is dropped when it is built, and that
    // degrades to "no seed", never to a refused or half-built page.
    QCOMPARE(response.status(), QStringLiteral("ok"));
    QVERIFY(!response.qml().isEmpty());
    QVERIFY(response.seed().isEmpty());
}

void tst_PageStore::edgeLogsAHookWithNoSeedFunctionOnlyOnce()
{
    // A hook whose seedFor is missing is reachable from the browser: a client can ask
    // for that page in a loop. If each failed request logged, the edge's log would grow
    // without bound on demand, which is a denial of service with extra steps. The hook
    // is checked once, when it is built, and then simply is not there.
    QTemporaryDir bundle{};
    QTemporaryDir pages{};
    QVERIFY(bundle.isValid() && pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");

    QQmlEngine engine;
    SynQt::WebEdgeConfig config{edgeConfigWithASeededPage(
        bundle.path(), pages.path(),
        QStringLiteral(REMOTE_PAGES_SRCDIR "/seeds/NoFunction.qml"))};
    SynQt::WebEdge edge{config, &engine};
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression{QStringLiteral("declares no seedFor")});
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));

    SynQt::Caller *caller{edgeCaller(edge, QStringLiteral("anonymous"), this)};
    {
        MessageCounter messages;
        for (int request{0}; request < 20; ++request) {
            const PageResponse response{edge.pagesService()->fetchPageFor(
                QStringLiteral("/c/summer"), QString{}, caller)};
            QCOMPARE(response.status(), QStringLiteral("ok"));
            QVERIFY(response.seed().isEmpty());
        }
        QCOMPARE(messages.count(), 0);
    }
}

void tst_PageStore::edgeRefusesASeedThatIsNotAnObject()
{
    // The client reads a seed as a JSON object. An array would parse to an empty map and
    // a string to nothing at all, so an author who returns the wrong shape gets silence
    // in both directions. Refuse it, say so once, and deliver the page with no seed.
    QTemporaryDir bundle{};
    QTemporaryDir pages{};
    QVERIFY(bundle.isValid() && pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");

    QQmlEngine engine;
    SynQt::WebEdgeConfig config{edgeConfigWithASeededPage(
        bundle.path(), pages.path(),
        QStringLiteral(REMOTE_PAGES_SRCDIR "/seeds/Array.qml"))};
    SynQt::WebEdge edge{config, &engine};
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));

    SynQt::Caller *caller{edgeCaller(edge, QStringLiteral("anonymous"), this)};
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression{QStringLiteral("did not return an object")});
    const PageResponse first{edge.pagesService()->fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, caller)};
    QCOMPARE(first.status(), QStringLiteral("ok"));
    QVERIFY(!first.qml().isEmpty());
    QVERIFY(first.seed().isEmpty());

    // Once per route, not once per request.
    {
        MessageCounter messages;
        for (int request{0}; request < 20; ++request) {
            const PageResponse response{edge.pagesService()->fetchPageFor(
                QStringLiteral("/c/summer"), QString{}, caller)};
            QVERIFY(response.seed().isEmpty());
        }
        QCOMPARE(messages.count(), 0);
    }
}

void tst_PageStore::edgeRefusesASeedNestedTooDeeply()
{
    // Converting an unbounded structure overflows the stack inside QJSValue::toVariant()
    // and QJsonDocument::fromVariant() and takes the whole edge down, for every connected
    // browser. The depth here (20000) is one that did exactly that. It is app-authored
    // code, but the realistic route to it is a hook serializing a tree whose depth
    // follows its data, so the seed is bounded rather than trusted.
    QTemporaryDir bundle{};
    QTemporaryDir pages{};
    QVERIFY(bundle.isValid() && pages.isValid());
    writePage(QDir{pages.path()}, QStringLiteral("C.qml"), "import QtQuick\nItem {}");

    QQmlEngine engine;
    SynQt::WebEdgeConfig config{edgeConfigWithASeededPage(
        bundle.path(), pages.path(),
        QStringLiteral(REMOTE_PAGES_SRCDIR "/seeds/Deep.qml"))};
    SynQt::WebEdge edge{config, &engine};
    QVERIFY2(edge.start(), qPrintable(edge.errorString()));

    SynQt::Caller *caller{edgeCaller(edge, QStringLiteral("anonymous"), this)};
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression{QStringLiteral("nested deeper than")});
    const PageResponse response{edge.pagesService()->fetchPageFor(
        QStringLiteral("/c/summer"), QString{}, caller)};

    // Still standing, and the page is still delivered: the seed is what degrades.
    QCOMPARE(response.status(), QStringLiteral("ok"));
    QVERIFY(!response.qml().isEmpty());
    QVERIFY(response.seed().isEmpty());
}

QTEST_MAIN(tst_PageStore)
#include "tst_pagestore.moc"
