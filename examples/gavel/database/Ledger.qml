// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative ledger on the database entity (docs/tutorial-hall-of-fame.md). It
// authorizes the CALLING ENTITY, not a user: only the web edge (Caller.entity === "web")
// may write, and it proves which entity it is with the certificate its mesh link
// presented. Any other entity (even one on the connect point's consumer allowlist) is
// refused here in the slot. This is a per_peer instance over mutual TLS, so it also
// requires Caller.isEntityVerified: the name is certificate-verified, never a
// colocation-trusted local-socket peer that merely presents "web".
//
// The Db helper (parameterized query/exec, so a value can never become SQL) backs the
// durable store when the persistence blueprint provisions it; this in-memory store keeps
// the connect-point contract identical while the tutorial's SQLite provider is wired in.
LedgerSource {
    id: ledger

    property var store: []

    function recordWinner(item, winner, amount) {
        if (!Caller.isEntityVerified || Caller.entity !== "web") {
            return;   // the database refuses any caller other than the verified edge
        }
        ledger.store.push({ item: item, winner: winner, amount: amount });
        ledger.count = ledger.store.length;
        ledger.winnerRecorded(item, winner, amount);   // announce to the edge
    }
}
