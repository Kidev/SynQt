// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The three privacy components as an app reaches them. `import SynQt`, then instantiate.
//
// It needs a scene graph, so it runs on the offscreen platform with the raster backend, and
// it is a separate binary from tst_privacy for the reason tests/graphics splits the same
// way. The accessor's own suite must run on a kit with no Qt Quick at all.

#include "privacy.h"
#include "session.h"
#include "synclientconfig.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUrl>

#include <memory>

using namespace SynQt;

namespace {

SynClientConfig withCookies()
{
    SynClientConfig config;
    config.privacyPolicyUrl = QStringLiteral("/privacy");
    config.legalNoticeUrl = QStringLiteral("/legal");
    config.privacyContact = QStringLiteral("privacy@example.com");
    config.retentionDays = 730;
    config.cookieCategories = {QStringLiteral("analytics")};
    config.erasureOffered = true;
    return config;
}

/// Stands in for the platform's browser: Qt.openUrlExternally hands it every https URL.
class ExternalBrowser : public QObject
{
    Q_OBJECT

public:
    QList<QUrl> opened;

public Q_SLOTS:
    void open(const QUrl &url)
    {
        opened.append(url);
    }
};

/// The footer's label that reads \a text, or null.
QQuickItem *labelReading(QQuickItem *footer, const QString &text)
{
    for (QQuickItem *child : footer->childItems()) {
        if (child->property("text").toString() == text) {
            return child;
        }
    }
    return nullptr;
}

/// Click \a item where a visitor would, in the window \a footer is shown in.
void click(QQuickItem *item)
{
    const QPointF centre{item->mapToScene(QPointF{item->width() / 2, item->height() / 2})};
    QTest::mouseClick(item->window(), Qt::LeftButton, Qt::NoModifier, centre.toPoint());
}

} // namespace

class PrivacyComponentsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        registerPrivacyTypes();
    }

    void init()
    {
        Privacy{withCookies()}.withdrawConsent();
    }

    void eachTypeResolvesFromTheSynQtModule()
    {
        // The registration hands qmlRegisterType a qrc URL, and the resource prefix that URL
        // names is written in src/client/CMakeLists.txt. Nothing else notices when the two
        // stop agreeing. The type registers, and instantiating it fails at runtime with a
        // file that does not exist.
        for (const QString &type : {QStringLiteral("LegalFooter"),
                                    QStringLiteral("CookieConsent"),
                                    QStringLiteral("DataErasureRequest")}) {
            QQmlApplicationEngine engine;
            Privacy privacy{withCookies(), &engine};
            Session session{withCookies(), &engine};
            engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);
            engine.rootContext()->setContextProperty(QStringLiteral("Session"), &session);
            QQmlComponent component{&engine};
            component.setData(QStringLiteral("import SynQt\nimport QtQuick\n%1 { }")
                                  .arg(type).toUtf8(), QUrl{});
            const std::unique_ptr<QObject> object{component.create()};
            QVERIFY2(object != nullptr,
                     qPrintable(type + QStringLiteral(": ") + component.errorString()));
        }
    }

    void theBannerIsInvisibleUntilAProjectDeclaresACategory()
    {
        QQmlApplicationEngine engine;
        // A project that declares no non-essential cookie. The session credential is exempt,
        // so there is nothing to ask and the banner never appears.
        Privacy privacy{SynClientConfig{}, &engine};
        engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);
        QQmlComponent component{&engine};
        component.setData("import SynQt\nimport QtQuick\nCookieConsent { }", QUrl{});
        const std::unique_ptr<QObject> object{component.create()};
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        QCOMPARE(object->property("visible").toBool(), false);
    }

    void theBannerAppearsForADeclaredCategoryAndGoesOnceAnswered()
    {
        QQmlApplicationEngine engine;
        Privacy privacy{withCookies(), &engine};
        engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);
        QQmlComponent component{&engine};
        component.setData("import SynQt\nimport QtQuick\nCookieConsent { }", QUrl{});
        const std::unique_ptr<QObject> object{component.create()};
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        QCOMPARE(object->property("visible").toBool(), true);
        privacy.acceptNecessaryOnly();
        QCOMPARE(object->property("visible").toBool(), false);
    }

    void erasureIsHiddenWhereNobodyUndertookToActOnIt()
    {
        QQmlApplicationEngine engine;
        SynClientConfig config{withCookies()};
        config.erasureOffered = false;
        Privacy privacy{config, &engine};
        Session session{config, &engine};
        engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);
        engine.rootContext()->setContextProperty(QStringLiteral("Session"), &session);
        QQmlComponent component{&engine};
        component.setData("import SynQt\nimport QtQuick\nDataErasureRequest { }", QUrl{});
        const std::unique_ptr<QObject> object{component.create()};
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        QCOMPARE(object->property("visible").toBool(), false);
    }

    void aPolicyLinkFollowsItsKindOfUrl_data()
    {
        QTest::addColumn<QString>("policy");
        QTest::addColumn<QString>("navigated");
        QTest::addColumn<QString>("opened");
        // An application route is the app's to navigate to. An absolute URL is a page the
        // router has no route for, and handing it to Router.go would land on the fallback.
        QTest::newRow("a route") << QStringLiteral("/privacy") << QStringLiteral("/privacy")
                                 << QString{};
        QTest::newRow("an absolute URL") << QStringLiteral("https://policies.example/privacy")
                                         << QString{}
                                         << QStringLiteral("https://policies.example/privacy");
    }

    void aPolicyLinkFollowsItsKindOfUrl()
    {
        QFETCH(QString, policy);
        QFETCH(QString, navigated);
        QFETCH(QString, opened);

        ExternalBrowser browser;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &browser, "open");
        const auto unsetHandler{qScopeGuard([]() {
            QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        })};

        QQmlApplicationEngine engine;
        SynClientConfig config{withCookies()};
        config.privacyPolicyUrl = policy;
        Privacy privacy{config, &engine};
        engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);
        QQmlComponent component{&engine};
        component.setData("import SynQt\nimport QtQuick\nLegalFooter { }", QUrl{});
        const std::unique_ptr<QObject> object{component.create()};
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        auto *footer{qobject_cast<QQuickItem *>(object.get())};
        QVERIFY(footer != nullptr);

        QQuickWindow window;
        window.resize(640, 80);
        footer->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QSignalSpy navigate{footer, SIGNAL(navigate(QString))};
        QQuickItem *link{labelReading(footer, QStringLiteral("Privacy policy"))};
        QVERIFY(link != nullptr);
        QTRY_VERIFY(link->width() > 0);
        click(link);

        QCOMPARE(navigate.count(), navigated.isEmpty() ? 0 : 1);
        if (!navigated.isEmpty()) {
            QCOMPARE(navigate.constFirst().constFirst().toString(), navigated);
        }
        QCOMPARE(browser.opened.size(), opened.isEmpty() ? 0 : 1);
        if (!opened.isEmpty()) {
            QCOMPARE(browser.opened.constFirst(), QUrl{opened});
        }
    }

    void hasConsentReEvaluatesWhenTheAnswerChanges()
    {
        // The reason hasConsent is a function-valued property and not a Q_INVOKABLE: written
        // as a call, a binding on it would be evaluated once, while the banner was still up,
        // and never again. This test fails if it is ever made a call.
        QQmlApplicationEngine engine;
        Privacy privacy{withCookies(), &engine};
        engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);
        QQmlComponent component{&engine};
        component.setData("import QtQuick\n"
                          "Item { property bool allowed: Privacy.hasConsent(\"analytics\") }",
                          QUrl{});
        const std::unique_ptr<QObject> object{component.create()};
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        QCOMPARE(object->property("allowed").toBool(), false);
        privacy.acceptAll();
        QCOMPARE(object->property("allowed").toBool(), true);
    }
};

QTEST_MAIN(PrivacyComponentsTest)

#include "tst_privacycomponents.moc"
