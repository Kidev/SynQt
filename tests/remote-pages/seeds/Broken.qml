// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// A hook that does not load at all: NotAType is nothing this engine knows. The page
// must still be delivered, with no seed.
PageSeed {
    NotAType {
    }
}
