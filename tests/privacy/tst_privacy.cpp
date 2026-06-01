// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The privacy accessor: what the project declared, what this visitor answered, and the two
// places where an answer is filtered against the declaration.

#include "privacy.h"
#include "synclientconfig.h"

#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace SynQt;

namespace {

SynClientConfig configured()
{
    SynClientConfig config;
    config.privacyPolicyUrl = QStringLiteral("/privacy");
    config.legalNoticeUrl = QStringLiteral("/legal");
    config.privacyContact = QStringLiteral("privacy@example.com");
    config.retentionDays = 730;
    config.cookieCategories = {QStringLiteral("analytics"), QStringLiteral("ads")};
    config.erasureOffered = true;
    return config;
}

void forgetAnswer()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

} // namespace

class PrivacyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Test mode moves QSettings under a throwaway prefix, so a run cannot read or write
        // whatever the developer's own applications have stored.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("SynQtTest"));
        QCoreApplication::setApplicationName(QStringLiteral("privacy"));
    }

    void init()
    {
        forgetAnswer();
    }

    void theBlockReachesQmlUnchanged()
    {
        const Privacy privacy{configured()};
        QCOMPARE(privacy.policyUrl(), QStringLiteral("/privacy"));
        QCOMPARE(privacy.legalNoticeUrl(), QStringLiteral("/legal"));
        QCOMPARE(privacy.contact(), QStringLiteral("privacy@example.com"));
        QCOMPARE(privacy.retentionDays(), 730);
        QCOMPARE(privacy.categories(), QStringList({QStringLiteral("analytics"),
                                                    QStringLiteral("ads")}));
        QVERIFY(privacy.isErasureOffered());
    }

    void aProjectWithNoCookiesAsksNothing()
    {
        // The session credential is exempt under Article 5(3), so a project that declares no
        // other cookie has nothing to ask about. This is what keeps the banner off every app
        // that never opted into one.
        SynClientConfig config;
        const Privacy privacy{config};
        QVERIFY(!privacy.isConsentRequired());
        QVERIFY(!privacy.isConsentAnswered());
        QVERIFY(privacy.granted().isEmpty());
        QVERIFY(!privacy.isErasureOffered());
    }

    void silenceIsNotConsent()
    {
        const Privacy privacy{configured()};
        QVERIFY(privacy.isConsentRequired());
        QVERIFY(!privacy.isConsentAnswered());
        QVERIFY(!privacy.hasConsent(QStringLiteral("analytics")));
        QVERIFY(!privacy.hasConsent(QStringLiteral("ads")));
    }

    void refusingIsAnAnswer()
    {
        Privacy privacy{configured()};
        QSignalSpy changed{&privacy, &Privacy::consentChanged};
        privacy.acceptNecessaryOnly();
        QCOMPARE(changed.count(), 1);
        // Answered, so the banner does not ask again, and nothing is permitted.
        QVERIFY(privacy.isConsentAnswered());
        QVERIFY(privacy.granted().isEmpty());
        QVERIFY(!privacy.hasConsent(QStringLiteral("analytics")));
    }

    void acceptingOneLeavesTheOther()
    {
        Privacy privacy{configured()};
        privacy.accept({QStringLiteral("analytics")});
        QVERIFY(privacy.hasConsent(QStringLiteral("analytics")));
        QVERIFY(!privacy.hasConsent(QStringLiteral("ads")));
    }

    void aCategoryNobodyDeclaredCannotBeGranted()
    {
        // A page that calls accept() with a name of its own invents permission for something
        // the project never wrote down, and would then read it back as consent.
        Privacy privacy{configured()};
        privacy.accept({QStringLiteral("analytics"), QStringLiteral("fingerprinting")});
        QCOMPARE(privacy.granted(), QStringList({QStringLiteral("analytics")}));
        QVERIFY(!privacy.hasConsent(QStringLiteral("fingerprinting")));
    }

    void theAnswerSurvivesARestart()
    {
        {
            Privacy privacy{configured()};
            privacy.accept({QStringLiteral("ads")});
        }
        const Privacy reopened{configured()};
        QVERIFY(reopened.isConsentAnswered());
        QCOMPARE(reopened.granted(), QStringList({QStringLiteral("ads")}));
    }

    void aWithdrawnAnswerAsksAgain()
    {
        Privacy privacy{configured()};
        privacy.acceptAll();
        QVERIFY(privacy.isConsentAnswered());
        QSignalSpy changed{&privacy, &Privacy::consentChanged};
        privacy.withdrawConsent();
        QCOMPARE(changed.count(), 1);
        QVERIFY(!privacy.isConsentAnswered());
        QVERIFY(privacy.granted().isEmpty());
        // And it is forgotten rather than only hidden, so the next launch asks too.
        const Privacy reopened{configured()};
        QVERIFY(!reopened.isConsentAnswered());
    }

    void aStoredAnswerCannotOutliveTheCategory()
    {
        // The project removed "ads" between launches. What is on disk still names it, and a
        // page asking whether it is permitted has to be told no. Nobody consented to the
        // category this project has now. They consented to the one it had before.
        {
            Privacy privacy{configured()};
            privacy.acceptAll();
        }
        SynClientConfig narrowed{configured()};
        narrowed.cookieCategories = {QStringLiteral("analytics")};
        const Privacy reopened{narrowed};
        QCOMPARE(reopened.granted(), QStringList({QStringLiteral("analytics")}));
        QVERIFY(!reopened.hasConsent(QStringLiteral("ads")));
    }
};

QTEST_MAIN(PrivacyTest)

#include "tst_privacy.moc"
