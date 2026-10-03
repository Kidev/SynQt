// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// An owner implemented in C++: a class deriving from the generated helper and overriding the
// implementation overload of each slot. The wire slot stays the helper's, so every check it
// makes still runs in front of the C++ code, and a slot the class leaves alone still reaches
// the owner's QML.

#include "caller.h"
#include "sessionmanager.h"
#include "sourcefactory.h"

#include "stock_sourcehelper.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

using namespace SynQt;

namespace {

constexpr int OneMinuteTtl{1};

/// The author's class, as a C++ owner writes it: overrides only, no Q_INVOKABLE. Its base is
/// what SynQt::Service derives from.
class Stock : public StockSourceBase
{
    Q_OBJECT

public:
    explicit Stock(QObject *parent = nullptr)
        : StockSourceBase{parent}
    {
    }

    int restocks{0};
    int clears{0};
    QString lastEntity;
    bool lastHadCaller{false};

protected:
    void restock(SynQt::Caller *caller, const QString &sku, int count) override
    {
        restocks += 1;
        lastHadCaller = caller != nullptr;
        lastEntity = caller != nullptr ? caller->entity() : QString{};
        m_total += count;
        // Copy-initialized: braces would pick the initializer-list constructor and
        // wrap the rows in one more list.
        QVariantList rows = shelfRows();
        rows.append(QVariantMap{{QStringLiteral("sku"), sku},
                                {QStringLiteral("count"), count}});
        setShelf(rows);
        emit restocked(sku, count);
    }

    int total(SynQt::Caller *caller) override
    {
        Q_UNUSED(caller)
        return m_total;
    }

    void clear(SynQt::Caller *caller) override
    {
        Q_UNUSED(caller)
        clears += 1;
        m_total = 0;
        setShelf(QVariantList{});
    }

private:
    int m_total{0};
};

} // namespace

class TestCppOwner : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        synqtRegisterStockSources();
        // After the helper's registration, as a generated main does: the last one wins.
        qmlRegisterType<Stock>("SynQt", 1, 0, "Stock");
    }

    void init()
    {
        m_sessions = std::make_unique<SessionManager>(QStringLiteral("anonymous"),
                                                      OneMinuteTtl);
    }

    /// The C++ implementation answers, handed the Caller the call arrived with.
    void aSlotReachesTheCppImplementationWithItsCaller()
    {
        Stock stock;
        Caller *caller{Caller::forEntity(QStringLiteral("Stock"), QStringLiteral("gate"),
                                         true, &stock, &stock)};
        SourceFactory::bindCaller(&stock, caller);
        QSignalSpy restocked{&stock, &StockSourceHelper::restocked};

        stock.StockSourceHelper::restock(QStringLiteral("SKU-1"), 4);

        QCOMPARE(stock.restocks, 1);
        QVERIFY(stock.lastHadCaller);
        QCOMPARE(stock.lastEntity, QStringLiteral("gate"));
        QCOMPARE(restocked.count(), 1);
        QCOMPARE(stock.shelf()->rowCount(), 1);
        QCOMPARE(stock.StockSourceHelper::total(), 4);
    }

    /// A bound is the helper's, so it refuses before the C++ code is reached.
    void aBoundStillRefusesBeforeTheCppImplementation()
    {
        Stock stock;
        Caller *caller{Caller::forEntity(QStringLiteral("Stock"), QStringLiteral("gate"),
                                         true, &stock, &stock)};
        SourceFactory::bindCaller(&stock, caller);

        QTest::ignoreMessage(QtWarningMsg,
                             "Stock.restock: 'sku' is declared string[16] and the value is "
                             "17 characters; refused");
        stock.StockSourceHelper::restock(QStringLiteral("SKU-0123456789ABC"), 1);
        QCOMPARE(stock.restocks, 0);

        stock.StockSourceHelper::restock(QStringLiteral("SKU-0123456789AB"), 1);
        QCOMPARE(stock.restocks, 1);
    }

    /// So is a scope gate: a caller without the scope never reaches the C++ code, and one
    /// with it does.
    void aScopeGateStillRefusesBeforeTheCppImplementation()
    {
        Stock stock;
        const QByteArray visitor{m_sessions->createSession(QStringLiteral("anonymous"))};
        Caller *caller{Caller::forUser(QStringLiteral("Stock"), m_sessions.get(), visitor,
                                       &stock, &stock)};
        SourceFactory::bindCaller(&stock, caller);

        QTest::ignoreMessage(QtWarningMsg,
                             "Stock.clear: refused, the caller does not hold the scope it "
                             "needs");
        stock.StockSourceHelper::clear();
        QCOMPARE(stock.clears, 0);

        const QByteArray admin{m_sessions->createSession(QStringLiteral("admin"))};
        Stock other;
        Caller *adminCaller{Caller::forUser(QStringLiteral("Stock"), m_sessions.get(), admin,
                                            &other, &other)};
        SourceFactory::bindCaller(&other, adminCaller);
        other.StockSourceHelper::clear();
        QCOMPARE(other.clears, 1);
    }

    /// A Source with no Caller bound hands its implementation nobody, and the gate on
    /// `clear` refuses it outright.
    void noCallerIsHandedOverAsNull()
    {
        Stock stock;
        stock.StockSourceHelper::restock(QStringLiteral("SKU-1"), 1);
        QCOMPARE(stock.restocks, 1);
        QVERIFY(!stock.lastHadCaller);

        QTest::ignoreMessage(QtWarningMsg,
                             "Stock.clear: refused, the caller does not hold the scope it "
                             "needs");
        stock.StockSourceHelper::clear();
        QCOMPARE(stock.clears, 0);
    }

    /// A shared entity: each caller's mirror forwards to the one C++ owner, which sees the
    /// Caller of whoever is calling right now.
    void aMirrorForwardsToTheSharedCppOwnerWithItsCaller()
    {
        Stock shared;
        Caller *sharedCaller{Caller::forEntity(QStringLiteral("Stock"), QString{}, false,
                                               &shared, &shared)};
        SourceFactory::bindCaller(&shared, sharedCaller);
        SourceFactory::holdsSharedState(&shared);

        StockSourceHelper mirror;
        Caller *caller{Caller::forEntity(QStringLiteral("Stock"), QStringLiteral("till"),
                                         true, &mirror, &mirror)};
        QVERIFY(SourceFactory::mirror(&mirror, &shared, caller));

        mirror.restock(QStringLiteral("SKU-2"), 2);
        QCOMPARE(shared.restocks, 1);
        QCOMPARE(shared.lastEntity, QStringLiteral("till"));
        QCOMPARE(mirror.total(), 2);
        // The mirror republishes what the C++ owner published.
        QCOMPARE(mirror.shelf()->rowCount(), 1);
    }

    /// The hybrid: a QML file rooted at the C++ class. A slot C++ leaves alone reaches the
    /// QML function of that name, the QML sees the Caller in its context, and QML
    /// properties sit beside the C++ ones on the same object.
    void aQmlFileRootedAtTheCppClassImplementsWhatCppLeavesAlone()
    {
        QQmlEngine engine;
        Stock *probe{nullptr};
        QQmlContext context{engine.rootContext()};
        std::unique_ptr<Caller> caller{Caller::forEntity(
            QStringLiteral("Stock"), QStringLiteral("gate"), true, nullptr)};
        context.setContextProperty(QStringLiteral("Caller"), caller.get());

        QQmlComponent component{&engine};
        component.setData(QByteArrayLiteral(
                              "import SynQt\n"
                              "Stock {\n"
                              "    property string lastNote: \"\"\n"
                              "    headline: \"from QML\"\n"
                              "    function note(text: string) {\n"
                              "        lastNote = text + \" by \" + Caller.entity;\n"
                              "    }\n"
                              "}\n"),
                          QUrl::fromLocalFile(QStringLiteral("/synqt-test/Stock.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object{component.create(&context)};
        QVERIFY2(object, qPrintable(component.errorString()));
        probe = qobject_cast<Stock *>(object.get());
        QVERIFY(probe != nullptr);
        caller->setSource(probe);
        SourceFactory::bindCaller(probe, caller.get());

        QCOMPARE(probe->headline(), QStringLiteral("from QML"));
        probe->StockSourceHelper::note(QStringLiteral("count again"));
        QCOMPARE(object->property("lastNote").toString(),
                 QStringLiteral("count again by gate"));
        // C++ still answers what it implements, on the same object.
        probe->StockSourceHelper::restock(QStringLiteral("SKU-3"), 3);
        QCOMPARE(probe->restocks, 1);
        QCOMPARE(probe->StockSourceHelper::total(), 3);
    }

    /// With no QML function and no C++ override, a slot does nothing and answers a default.
    void aSlotNobodyImplementsIsANoOp()
    {
        Stock stock;
        stock.StockSourceHelper::note(QStringLiteral("nobody listens"));
        QCOMPARE(stock.restocks, 0);
    }

private:
    std::unique_ptr<SessionManager> m_sessions;
};

QTEST_MAIN(TestCppOwner)

#include "tst_cppowner.moc"
