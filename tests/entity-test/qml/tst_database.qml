// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The database's own Source, in the same process where `Database` is also the neighbour
// the Ledger reads. The one registered type has to be both.
import QtQuick
import QtTest
import SynQt.Test

TestCase {
    name: "DatabaseSlots"

    EntityTest {
        id: harness

        source: "../database/Database.qml"
    }

    SignalSpy {
        id: recorded

        target: harness.subject
        signalName: "recorded"
    }

    function init() {
        verify(harness.load(), harness.errorString);
        recorded.clear();
    }

    function test_the_database_is_its_own_source() {
        harness.callerIsEntity("web");
        harness.subject.recordWinner("vase", "bob", 300);
        compare(harness.subject.count, 1);
        compare(recorded.count, 1);
        compare(recorded.signalArguments[0][0], "vase");
    }
}
