// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What an application's own test looks like: QML, QtTest, and the SynQt.Test harness.
// Nothing here is C++, and nothing here knows about sockets, certificates or a database
// file. The Source is the real one; the Caller is the real one; only the engine behind
// `Db` is in memory.
import QtQuick
import QtTest
import SynQt.Test

TestCase {
    id: suite

    name: "LedgerSlots"

    EntityTest {
        id: harness

        source: "../web/Ledger.qml"
        schema: "../database/schema.sql"
    }

    SignalSpy {
        id: rejections

        target: harness.subject
        signalName: "bidRejected"
    }

    // A fresh Source and a fresh database per test function, so no test can pass only
    // because of the order it ran in.
    function init() {
        verify(harness.load(), harness.errorString);
        rejections.clear();
    }

    function test_the_harness_loads_the_real_source() {
        verify(harness.subject !== null);
        compare(harness.contract, "Ledger");
        compare(harness.subject.highBid, 100);
    }

    function test_an_anonymous_visitor_cannot_bid() {
        harness.callerIsUser("anonymous");
        harness.subject.placeBid(500, "mallory");
        compare(harness.subject.highBid, 100);
        compare(rejections.count, 1);
        compare(rejections.signalArguments[0][0], "Sign in to bid.");
    }

    function test_a_signed_in_user_can_outbid() {
        harness.callerIsUser("user", { sub: "alice", login: "alice" });
        harness.subject.placeBid(150, "alice");
        compare(harness.subject.highBid, 150);
        compare(rejections.count, 0);
    }

    function test_a_lower_bid_is_refused_and_the_bidder_told_why() {
        harness.callerIsUser("user", { sub: "alice" });
        harness.subject.placeBid(50, "alice");
        compare(harness.subject.highBid, 100);
        compare(rejections.count, 1);
        compare(rejections.signalArguments[0][0], "Bid must beat 100.");
    }

    // The scope order is hierarchical by default, exactly as a scaffolded project's is,
    // so a moderator satisfies a "user" check without the test saying so.
    function test_a_higher_scope_satisfies_a_lower_check() {
        harness.callerIsUser("moderator", { sub: "mod" });
        harness.subject.placeBid(200, "mod");
        compare(harness.subject.highBid, 200);
    }

    function test_only_the_edge_may_record_a_winner() {
        harness.callerIsEntity("rogue");
        compare(harness.subject.recordWinner("vase", "bob", 300), false);
        compare(harness.dbQuery("SELECT * FROM winners").length, 0);

        harness.callerIsEntity("web");
        compare(harness.subject.recordWinner("vase", "bob", 300), true);
        const rows = harness.dbQuery("SELECT item, winner, amount FROM winners");
        compare(rows.length, 1);
        compare(rows[0].item, "vase");
        compare(rows[0].amount, 300);
    }

    // A user is not an entity and never satisfies an entity check, whatever their scope.
    function test_an_admin_user_is_still_not_the_edge() {
        harness.callerIsUser("admin", { sub: "root" });
        compare(harness.subject.recordWinner("vase", "bob", 300), false);
    }

    function test_each_test_starts_from_an_empty_database() {
        harness.callerIsEntity("web");
        harness.subject.recordWinner("first", "bob", 1);
        compare(harness.dbQuery("SELECT * FROM winners").length, 1);
        verify(harness.load(), harness.errorString);
        compare(harness.dbQuery("SELECT * FROM winners").length, 0);
    }
}
