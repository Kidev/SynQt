// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The vocabulary makeGatedConfig() declares, as the enum a real project's mapping hook is
// generated beside (tools/synqt/synqt/scopegen.py writes this file for an app). A member's
// value is its index in scopes.order, which is also its authority rank:
//   0 = anonymous
//   1 = user
//   2 = moderator
import QtQml

QtObject {
    enum Value { Anonymous, User, Moderator }
}
