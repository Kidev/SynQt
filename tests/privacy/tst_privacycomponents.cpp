// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The three privacy components as an app reaches them. `import SynQt`, then instantiate.
//
// It needs a scene graph, so it runs on the offscreen platform with the raster backend, and
// it is a separate binary from tst_privacy for the reason tests/graphics splits the same
// way. The accessor's own suite must run on a kit with no Qt Quick at all.

#include "privacy.h"
#include "synclientconfig.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

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

} // namespace

class PrivacyComponentsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("SynQtTest"));
        QCoreApplication::setApplicationName(QStringLiteral("privacycomponents"));
        registerPrivacyTypes();
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
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
            engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);
            engine.rootContext()->setContextProperty(QStringLiteral("Session"),
                                                     static_cast<QObject *>(nullptr));
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
        engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);
        engine.rootContext()->setContextProperty(QStringLiteral("Session"),
                                                 static_cast<QObject *>(nullptr));
        QQmlComponent component{&engine};
        component.setData("import SynQt\nimport QtQuick\nDataErasureRequest { }", QUrl{});
        const std::unique_ptr<QObject> object{component.create()};
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        QCOMPARE(object->property("visible").toBool(), false);
    }

    void hasConsentReEvaluatesWhenTheAnswerChanges()
    {
        // The reason hasConsent is a function-valued property and not a Q_INVOKABLE: written
        // as a call, a binding on it would be evaluated once, while the banner was still up,
        // and never again. This is the test that fails if it is ever changed back.
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
