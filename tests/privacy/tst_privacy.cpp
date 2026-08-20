// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The privacy accessor: what the project declared, what this visitor answered, and the two
// places where an answer is filtered against the declaration.

#include "privacy.h"
#include "synclientconfig.h"

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUrl>

using namespace SynQt;

namespace {

/// Where every test's visitor has been, unless it says otherwise.
const QUrl edgeOne{QStringLiteral("wss://one.example:8443/sync")};
/// Another project's edge.
const QUrl edgeTwo{QStringLiteral("wss://two.example:8443/sync")};

SynClientConfig configured()
{
    SynClientConfig config;
    config.edgeUrl = edgeOne;
    config.privacyPolicyUrl = QStringLiteral("/privacy");
    config.legalNoticeUrl = QStringLiteral("/legal");
    config.privacyContact = QStringLiteral("privacy@example.com");
    config.retentionDays = 730;
    config.cookieCategories = {QStringLiteral("analytics"), QStringLiteral("ads")};
    config.erasureOffered = true;
    return config;
}

SynClientConfig at(const QUrl &edgeUrl)
{
    SynClientConfig config{configured()};
    config.edgeUrl = edgeUrl;
    return config;
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
        // The generated client names no organization, and a default QSettings keeps nothing
        // without one on WebAssembly or Windows. The accessor has to keep the answer anyway.
        QVERIFY(QCoreApplication::organizationName().isEmpty());
    }

    void init()
    {
        Privacy{configured()}.withdrawConsent();
        Privacy{at(edgeTwo)}.withdrawConsent();
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

    void anAnswerBelongsToTheEdgeItWasGivenTo()
    {
        // Every scaffolded desktop client is an executable called `app`, so two projects on
        // one account share an application name. Consent is given to one controller, and the
        // other one's banner still has to ask.
        {
            Privacy privacy{configured()};
            privacy.acceptAll();
        }
        const Privacy elsewhere{at(edgeTwo)};
        QVERIFY(!elsewhere.isConsentAnswered());
        QVERIFY(!elsewhere.hasConsent(QStringLiteral("analytics")));
        const Privacy here{configured()};
        QVERIFY(here.hasConsent(QStringLiteral("analytics")));
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
