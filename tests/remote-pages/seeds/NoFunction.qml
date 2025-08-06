// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// A hook an author got wrong: it loads, but it never declares seedFor. The page must
// still be delivered, with no seed, rather than the edge failing the request.
PageSeed {
    readonly property string note: "seedFor is missing on purpose"
}
