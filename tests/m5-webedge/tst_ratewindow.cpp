// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The rate-window table on its own, which is where the case that matters is.
//
// Three gates in this framework ration by client address. The monitor's password route, the
// device-credential route, and the API server. Each keeps a table keyed by address, and each
// therefore has to answer the question this file is about: what happens when the table gets
// too big? The obvious answer is to empty it, and the obvious answer turns the ceiling into
// a reset primitive. An attacker who can present addresses -- an IPv6 /64 is a practically
// unlimited supply, and so is a forwarding header on a deployment that trusts one -- fills
// the table with entries they will never use again, the table is emptied, and their count
// against the address they are actually guessing from goes back to zero.
//
// A test through a real edge cannot reach this: it would have to arrive from four thousand
// addresses. The table is a pure function of what is in it and what time it is, which is
// why it is tested here instead, the way the client-address resolver next to it is.

#include "ratewindow.h"

#include <QTest>

using SynQt::pruneRateWindows;
using SynQt::RateWindow;

namespace {

QString addressOf(int index)
{
    return QStringLiteral("10.%1.%2.%3")
        .arg((index >> 16) & 0xff)
        .arg((index >> 8) & 0xff)
        .arg(index & 0xff);
}

} // namespace

class TestRateWindow : public QObject
{
    Q_OBJECT

private slots:
    void aTableUnderTheCapIsLeftAlone()
    {
        QHash<QString, RateWindow> windows;
        windows.insert(QStringLiteral("10.0.0.1"), RateWindow{1000, 7});
        QVERIFY(!pruneRateWindows(windows, 2000, 60000, 4096));
        QCOMPARE(windows.size(), 1);
        QCOMPARE(windows.value(QStringLiteral("10.0.0.1")).count, 7);
    }

    void aFullTableOfExpiredWindowsIsCleared()
    {
        QHash<QString, RateWindow> windows;
        for (int index{0}; index < 4096; ++index) {
            windows.insert(addressOf(index), RateWindow{1000, 1});
        }
        // Long past the window, so none of these is anybody's live budget.
        QVERIFY(!pruneRateWindows(windows, 1000 + 60001, 60000, 4096));
        QCOMPARE(windows.size(), 0);
    }

    // The one that matters. A guesser fills the table from addresses they will never use
    // again and their own count survives it, because nothing that is still inside its
    // window is dropped. Clearing the table wholesale is what would hand them a fresh
    // budget on demand, however many attempts they had already spent.
    void aFloodOfLiveAddressesDoesNotClearTheGuessersOwnCount()
    {
        QHash<QString, RateWindow> windows;
        const QString guesser{QStringLiteral("203.0.113.9")};
        windows.insert(guesser, RateWindow{1000, 10});
        for (int index{0}; index < 5000; ++index) {
            windows.insert(addressOf(index), RateWindow{1000, 1});
        }

        // Every window is live, so pruning frees nothing and the helper says so: the caller
        // is expected to refuse rather than forget.
        QVERIFY2(pruneRateWindows(windows, 1500, 60000, 4096),
                 "a table of live windows must report that it is still over its cap");
        QCOMPARE(windows.value(guesser).count, 10);
    }

    // The mixed case, which is what a real table looks like. Some windows have run out and
    // some have not. What is dropped is only the ones that have.
    void onlyTheWindowsThatRanOutAreDropped()
    {
        QHash<QString, RateWindow> windows;
        for (int index{0}; index < 3000; ++index) {
            windows.insert(addressOf(index), RateWindow{0, 1});          // long gone
        }
        for (int index{3000}; index < 5000; ++index) {
            windows.insert(addressOf(index), RateWindow{60000, 4});      // still live
        }
        QVERIFY(!pruneRateWindows(windows, 61000, 60000, 4096));
        QCOMPARE(windows.size(), 2000);
        QCOMPARE(windows.value(addressOf(4999)).count, 4);
    }
};

QTEST_MAIN(TestRateWindow)
#include "tst_ratewindow.moc"
