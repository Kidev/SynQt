// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Who may read the console. The monitor holds every entity's record, so a single sign-in
// here reveals what the whole system has been doing. That is why it has an identity system
// of its own and why the tests below are about what it refuses.

#include "operatorstore.h"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTest>

using namespace SynQt;

class TestOperator : public QObject
{
    Q_OBJECT

private slots:
    void aMintedCredentialAcceptsItsPasswordAndRefusesEveryOther()
    {
        OperatorStore store;
        QVERIFY(store.add(OperatorStore::mint(QStringLiteral("ada"),
                                              QStringLiteral("correct horse battery"))));

        QVERIFY(store.verify(QStringLiteral("ada"), QStringLiteral("correct horse battery")));
        QVERIFY(!store.verify(QStringLiteral("ada"), QStringLiteral("correct horse batter")));
        QVERIFY(!store.verify(QStringLiteral("ada"), QString{}));
        QVERIFY(!store.verify(QStringLiteral("mallory"),
                              QStringLiteral("correct horse battery")));
    }

    void thePasswordIsNowhereInWhatIsStored()
    {
        const QString password{QStringLiteral("correct horse battery")};
        const QString stored{OperatorStore::mint(QStringLiteral("ada"), password)};
        // Not the password, and not anything derived from it that a reader could reverse:
        // a salt and a PBKDF2 hash, both hex.
        QVERIFY(!stored.contains(password));
        const QStringList parts{stored.split(QLatin1Char(':'))};
        QCOMPARE(parts.size(), 4);
        QCOMPARE(parts.at(0), QStringLiteral("ada"));
        QVERIFY(parts.at(1).toInt() >= OperatorStore::MinimumIterations);
        QVERIFY(QRegularExpression{QStringLiteral("^[0-9a-f]+$")}.match(parts.at(2)).hasMatch());
        QVERIFY(QRegularExpression{QStringLiteral("^[0-9a-f]+$")}.match(parts.at(3)).hasMatch());
    }

    void everyOperatorGetsTheirOwnSalt()
    {
        // Two operators with the same password must not have the same stored hash, or one
        // stolen store tells whoever reads it which accounts share a password.
        const QString first{OperatorStore::mint(QStringLiteral("ada"), QStringLiteral("same"))};
        const QString second{OperatorStore::mint(QStringLiteral("bob"), QStringLiteral("same"))};
        QVERIFY(first.split(QLatin1Char(':')).at(2)
                != second.split(QLatin1Char(':')).at(2));
        QVERIFY(first.split(QLatin1Char(':')).at(3)
                != second.split(QLatin1Char(':')).at(3));
    }

    void aWeakerCredentialIsRefusedRatherThanWarnedAbout()
    {
        OperatorStore store;
        QString error;
        // Hand-written with a low iteration count, which is what a credential minted by an
        // older tool or by hand looks like. A weakness only mentioned in a log line stays
        // as weak as the day it was written.
        QVERIFY(!store.add(QStringLiteral("ada:1000:aabb:ccdd"), &error));
        QVERIFY2(error.contains(QStringLiteral("iterations")), qPrintable(error));
        QVERIFY(store.isEmpty());
    }

    void aMonitorWithNoOperatorsRefusesEverybody()
    {
        // The opposite default, letting everyone in until somebody is configured, is how
        // an operations console ends up on a network with no gate on it at all.
        OperatorStore store;
        QVERIFY(store.isEmpty());
        QVERIFY(!store.verify(QStringLiteral("ada"), QStringLiteral("anything")));
        QVERIFY(!store.verify(QString{}, QString{}));
    }

    void operatorsAreReadFromTheEnvironmentAndNotFromTheRepository()
    {
        // A credential in synqt.yaml is a credential in a repository. The monitor reads
        // its operators from its own environment, like every other secret in SynQt.
        const QString ada{OperatorStore::mint(QStringLiteral("ada"), QStringLiteral("one"))};
        const QString bob{OperatorStore::mint(QStringLiteral("bob"), QStringLiteral("two"))};
        qputenv(OperatorStore::credentialVariable(), (ada + QLatin1Char(' ') + bob).toUtf8());

        OperatorStore store;
        QString error;
        QVERIFY2(store.loadFromEnvironment(&error), qPrintable(error));
        QCOMPARE(store.names(), QStringList({QStringLiteral("ada"), QStringLiteral("bob")}));
        QVERIFY(store.verify(QStringLiteral("ada"), QStringLiteral("one")));
        QVERIFY(store.verify(QStringLiteral("bob"), QStringLiteral("two")));
        QVERIFY(!store.verify(QStringLiteral("ada"), QStringLiteral("two")));
        qunsetenv(OperatorStore::credentialVariable());
    }

    void oneMalformedEntryDoesNotLockEveryOperatorOut()
    {
        const QString ada{OperatorStore::mint(QStringLiteral("ada"), QStringLiteral("one"))};
        qputenv(OperatorStore::credentialVariable(),
                (ada + QStringLiteral(" nonsense")).toUtf8());

        OperatorStore store;
        QString error;
        // Reported, so somebody fixes it. The entries that parse are still loaded, because
        // one mistyped line locking every operator out of the console is an outage.
        QVERIFY(!store.loadFromEnvironment(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(store.verify(QStringLiteral("ada"), QStringLiteral("one")));
        qunsetenv(OperatorStore::credentialVariable());
    }

    void aWrongPasswordCostsTheSameWhicheverByteIsWrong()
    {
        // Not a timing proof, which a unit test cannot give. A check that the comparison
        // does not return on the first wrong byte. A monitor is reachable by whoever can
        // reach its port, and an early return tells them the answer one byte at a time.
        OperatorStore store;
        QVERIFY(store.add(OperatorStore::mint(QStringLiteral("ada"),
                                              QStringLiteral("correct horse"))));
        QElapsedTimer clock;
        clock.start();
        for (int index{0}; index < 3; ++index) {
            QVERIFY(!store.verify(QStringLiteral("ada"), QStringLiteral("xorrect horse")));
        }
        const qint64 early{clock.restart()};
        for (int index{0}; index < 3; ++index) {
            QVERIFY(!store.verify(QStringLiteral("ada"), QStringLiteral("correct hors_")));
        }
        const qint64 late{clock.elapsed()};
        // Both are dominated by the derivation, which is the point: the comparison is
        // nothing next to it, and neither answer is reached sooner than the other.
        QVERIFY2(qAbs(early - late) < (qMax(early, late) / 2 + 5),
                 qPrintable(QStringLiteral("first-byte %1ms vs last-byte %2ms")
                                .arg(early).arg(late)));
    }

    void aCredentialTheCliMintedIsOneTheMonitorAccepts()
    {
        // The two halves derive the same key or the feature does not work at all: the CLI
        // mints in Python (hashlib.pbkdf2_hmac) and the monitor verifies in C++
        // (QPasswordDigestor). This credential was produced by `synqt monitor operator add`
        // with a fixed salt. If either side ever changes its derivation, this goes red
        // instead of every operator being locked out of a deployed console.
        OperatorStore store;
        QString error;
        QVERIFY2(store.add(QStringLiteral(
            "ada:600000:000102030405060708090a0b0c0d0e0f:"
            "bb06c8c0b1dd5bfd4e40f4e297a2d0e64da7ef94b4b8ec20989021c8b41536ad"), &error),
                 qPrintable(error));
        QVERIFY(store.verify(QStringLiteral("ada"), QStringLiteral("correct horse battery")));
        QVERIFY(!store.verify(QStringLiteral("ada"), QStringLiteral("correct horse batter")));
    }
};

QTEST_GUILESS_MAIN(TestOperator)
#include "tst_operator.moc"
