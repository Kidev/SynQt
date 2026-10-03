// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtTest
import SynQt.Test

// The test runner does not compile a C++ entity's class, so its Source refuses to load and
// says why, instead of loading the generated helper in its place.
TestCase {
    name: "CppEntity"

    EntityTest {
        id: harness

        source: "../service/lobby/Lobby.qml"
    }

    function test_a_cpp_entity_refuses_to_load_and_says_why() {
        verify(!harness.load());
        verify(harness.errorString.indexOf("lobby is written in C++") >= 0,
               harness.errorString);
    }
}
