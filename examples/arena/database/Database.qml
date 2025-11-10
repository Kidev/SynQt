// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Singleton

import QtQuick

// The 'database' entity itself: one of it, for as long as the entity runs. State
// that belongs to the whole entity goes here rather than in a Source, because a
// Source can be created per session or per peer and anything shared has to
// outlive any one of them. Every Source this entity owns reaches it as `Database`.
QtObject {
    id: root
}
