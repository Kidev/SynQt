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

    /// An operator name that does not exist must cost what one that does costs.
    ///
    /// The gate answers one "no" to every failure, so that nothing learns which half of a
    /// guess was right. Returning early on an unknown name would give that away by the
    /// clock: a known name spends a 600,000-round PBKDF2 and an unknown one a string
    /// comparison, four orders of magnitude apart and readable over the network, so a
    /// guesser could enumerate the operators and then spend every attempt on a real name.
    ///
    /// Measured as a ratio with a wide floor rather than as a duration. What is ruled out
    /// is the difference between microseconds and a KDF, not a few percent of jitter.
    void anUnknownOperatorCostsWhatAKnownOneCosts()
    {
        OperatorStore store;
        QVERIFY(store.add(OperatorStore::mint(QStringLiteral("ada"),
                                              QStringLiteral("correct horse battery"))));

        // Warm first. The first derivation on a process pays for whatever the crypto
        // backend sets up, and charging that to one of the two answers below would be
        // measuring the warm-up rather than the gate.
        store.verify(QStringLiteral("ada"), QStringLiteral("wrong"));

        QElapsedTimer clock;
        clock.start();
        QVERIFY(!store.verify(QStringLiteral("ada"), QStringLiteral("wrong")));
        const qint64 known{clock.nsecsElapsed()};

        clock.restart();
        QVERIFY(!store.verify(QStringLiteral("mallory"), QStringLiteral("wrong")));
        const qint64 unknown{clock.nsecsElapsed()};

        QVERIFY2(unknown * 2 >= known,
                 qPrintable(QStringLiteral("an unknown operator was refused in %1 ns and a "
                                           "known one in %2 ns, so the clock says which "
                                           "names exist")
                                .arg(unknown).arg(known)));
    }

    // The same question with the store holding operators of unequal cost, the ordinary
    // state: they are minted one at a time and nothing makes their round counts agree. An
    // unknown name worked against whichever credential is listed first would, with the
    // cheap one first, be refused faster than the expensive operator ever is, and "that
    // name came back too quickly" is exactly what an enumerator looks for.
    void anUnknownOperatorCostsWhatTheDearestKnownOneCosts()
    {
        OperatorStore store;
        // Cheapest first, because this is the order that can leak.
        QVERIFY(store.add(OperatorStore::mint(QStringLiteral("ada"),
                                              QStringLiteral("correct horse battery"),
                                              OperatorStore::MinimumIterations)));
        QVERIFY(store.add(OperatorStore::mint(QStringLiteral("bob"),
                                              QStringLiteral("correct horse battery"),
                                              OperatorStore::MinimumIterations * 4)));

        store.verify(QStringLiteral("ada"), QStringLiteral("wrong"));  // warm, as above

        QElapsedTimer clock;
        clock.start();
        QVERIFY(!store.verify(QStringLiteral("bob"), QStringLiteral("wrong")));
        const qint64 dearest{clock.nsecsElapsed()};

        clock.restart();
        QVERIFY(!store.verify(QStringLiteral("mallory"), QStringLiteral("wrong")));
        const qint64 unknown{clock.nsecsElapsed()};

        // Half, for the same reason the test above uses a factor rather than an equality: a
        // shared runner's scheduler is noisier than the thing being measured. The failure
        // this catches is not subtle, since the two round counts differ fourfold.
        QVERIFY2(unknown * 2 >= dearest,
                 qPrintable(QStringLiteral("an unknown operator was refused in %1 ns and the "
                                           "most expensive known one in %2 ns, so the clock "
                                           "says which names exist")
                                .arg(unknown).arg(dearest)));
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
        // A monitor is reachable by whoever can reach its port, and an answer that comes
        // back sooner for a closer guess tells them the password one byte at a time. This
        // is not a timing proof (a unit test cannot give one): it checks that a guess wrong
        // in its first byte and one wrong in its last both pay the whole derivation.
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
        // Both are dominated by the derivation. The comparison after it reads derived bytes
        // a guesser cannot steer, and it is constant time as well.
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
