// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The client update default: an update the app does not handle is applied immediately,
// and an app that handles it owns the timing. The browser half (the service worker and
// the Emscripten bridge) needs a real WebAssembly build; this pins the decision itself,
// which is the part that must never regress silently.

#include "clientupdate.h"

#include <QObject>
#include <QSignalSpy>
#include <QTest>

using SynQt::ClientUpdate;

namespace {

// Records the reload instead of asking the browser for one, so the default is testable
// without a browser.
class RecordingUpdate : public ClientUpdate
{
public:
    int reloads{0};

protected:
    void reloadPage() override { ++reloads; }
};

} // namespace

class TestClientUpdate : public QObject
{
    Q_OBJECT

private slots:
    void appliesImmediatelyWhenTheAppIgnoresTheSignal()
    {
        RecordingUpdate update;
        update.notifyUpdateReady();
        // Nothing is connected to updateReady, so an update the app will never apply must
        // not be left pending forever.
        QCOMPARE(update.reloads, 1);
    }

    void defersToTheAppWhenItHandlesTheSignal()
    {
        RecordingUpdate update;
        QSignalSpy spy{&update, &ClientUpdate::updateReady};
        update.notifyUpdateReady();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(update.reloads, 0);  // the app decides when
    }

    void applyUpdateReloadsOnDemand()
    {
        RecordingUpdate update;
        QSignalSpy spy{&update, &ClientUpdate::updateReady};
        update.notifyUpdateReady();
        QCOMPARE(update.reloads, 0);
        update.applyUpdate();
        QCOMPARE(update.reloads, 1);
    }

    void aDisconnectedHandlerFallsBackToTheDefault()
    {
        // A handler that goes away (a destroyed QML scope) must not strand the update.
        RecordingUpdate update;
        {
            QSignalSpy spy{&update, &ClientUpdate::updateReady};
            update.notifyUpdateReady();
            QCOMPARE(update.reloads, 0);
        }
        update.notifyUpdateReady();
        QCOMPARE(update.reloads, 1);
    }
};

QTEST_MAIN(TestClientUpdate)
#include "tst_clientupdate.moc"
