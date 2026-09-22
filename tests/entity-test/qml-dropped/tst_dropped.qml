// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Run on its own by the entity-test-dropped-call ctest, which passes only when this test
// fails with the facade's warning. A test that reaches a neighbour the harness never
// connects must not pass over a call that went nowhere.
import QtQuick
import QtTest
import SynQt.Test

TestCase {
    name: "DroppedCall"

    EntityTest {
        id: harness

        source: "../web/Ledger.qml"
        schema: "../database/schema.sql"
    }

    function test_a_call_into_the_database_fails_this_test() {
        verify(harness.load(), harness.errorString);
        harness.callerIsUser("user", { sub: "alice" });
        harness.subject.forwardToDatabase("vase");
    }
}
