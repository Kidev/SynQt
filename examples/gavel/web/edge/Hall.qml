// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The Hall of Fame the edge owns and the browser sees (docs/tutorial-hall-of-fame.md). The
// browser must never reach the books entity directly, so the edge holds this live list and
// fills it from the books entity's ledger.
//
// One of these per browser session, like every connect point Source. The list itself is not
// per session, so it lives in the `Edge` singleton and this publishes it: one binding, so a
// new winner arriving at the entity reaches every session's Source with nothing else
// written. `winnersRows` keeps only the roles the contract declares, so nothing the ledger
// holds beyond them reaches a browser.
Hall {
    winnersRows: Edge.winners
}
