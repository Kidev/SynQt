// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Test data, written by hand here, matching byte for byte what synqt.scopegen generates
// beside a real project's mapping hook (tools/synqt/tests/test_scopegen.py asserts the
// shape). It sits in this directory because a QML component resolves an unqualified type
// against its own directory first, which is what lets map.qml say Scope.Moderator
// with no import at all.
//
// A member's value is its index in scopes.order, which is also its authority rank:
//   0 = anonymous
//   1 = user
//   2 = moderator
//   3 = admin
import QtQml

QtObject {
    enum Value { Anonymous, User, Moderator, Admin }
}
