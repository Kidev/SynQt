// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What an application's own test looks like: QML, QtTest, and the SynQt.Test harness.
// Nothing here is C++, and nothing here knows about sockets, certificates or a database
// file. The Source is the real one. The Caller is the real one. Only the engine behind
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

    // The harness installs each entity the Source consumes as the runtime does before the
    // link opens: the accessor and its handlers resolve, and it is never ready. Without
    // that, a Source that reads a neighbour does not load at all.
    function test_a_consumed_entity_is_there_and_never_ready() {
        compare(harness.subject.databaseReachable, false);
        compare(harness.subject.lastRecorded, "");
    }

    // A slot that hands work to that neighbour reaches nothing. That limit is documented
    // (docs/testing.md), and what makes it safe to have is that it is loud: the call warns,
    // and the warning fails the test that made it (the entity-test-dropped-call ctest). Here
    // the warning is expected, which is also how a test accepts a call it knows is dropped.
    function test_reaching_another_entity_fails_rather_than_passing_quietly() {
        harness.callerIsUser("user", { sub: "alice" });
        ignoreWarning(new RegExp("^SynQt: the 'Database' connect point is not available, "
                                 + "so recordWinner\\(\\) was not sent$"));
        harness.subject.forwardToDatabase("vase");
        compare(harness.dbQuery("SELECT * FROM winners").length, 0);
    }

    function test_each_test_starts_from_an_empty_database() {
        harness.callerIsEntity("web");
        harness.subject.recordWinner("first", "bob", 1);
        compare(harness.dbQuery("SELECT * FROM winners").length, 1);
        verify(harness.load(), harness.errorString);
        compare(harness.dbQuery("SELECT * FROM winners").length, 0);
    }

    // The cache, the document store and the scheduler are reset with the database. A value
    // one test cached, a document it inserted or a timer it started would otherwise decide
    // what the next test sees.
    function test_each_test_starts_from_empty_helpers() {
        harness.subject.keep("k");
        harness.subject.tickEvery(5);
        compare(harness.cacheValue("k"), "kept");
        compare(harness.subject.notesKept(), 1);
        tryVerify(() => harness.recorded().some(event => event.message === "tick"));

        verify(harness.load(), harness.errorString);
        compare(harness.cacheValue("k"), undefined);
        compare(harness.subject.notesKept(), 0);
        wait(50);
        compare(harness.recorded().filter(event => event.message === "tick").length, 0);
    }

    // What an entity says about itself is testable like anything else it does. `Log` is in
    // scope in every entity, whatever its type, unlike `Db` or `Cache`.
    function test_an_entity_says_what_it_did_in_its_own_words() {
        harness.callerIsUser("user", { sub: "alice" });
        harness.subject.placeBid(150, "alice");

        const said = harness.recorded().filter(event => event.message === "bid accepted");
        compare(said.length, 1);
        compare(said[0].attributes.amount, 150);
        compare(said[0].attributes.bidder, "alice");
        // The entity's own log line, at info level.
        compare(said[0].categoryName, "application");
        compare(said[0].severityName, "info");
    }

    function test_a_refused_bid_says_nothing_and_the_record_shows_it() {
        harness.callerIsUser("anonymous");
        harness.subject.placeBid(500, "mallory");
        compare(harness.recorded().filter(e => e.message === "bid accepted").length, 0);
    }

    function test_an_entity_cannot_claim_to_be_another_one() {
        harness.callerIsUser("user", { sub: "alice" });
        // The attribute is written by the entity. The name is stamped by the runtime past
        // anything QML can reach, so this is the one thing an entity cannot forge about
        // itself. Whatever it puts in the map, the record says who said it.
        harness.subject.placeBid(150, "alice");
        const said = harness.recorded().filter(event => event.message === "bid accepted");
        compare(said.length, 1);
        compare(said[0].entity, "test");
    }

    function test_what_one_test_said_is_not_read_by_the_next() {
        harness.callerIsUser("user", { sub: "alice" });
        harness.subject.placeBid(150, "alice");
        compare(harness.recorded().filter(e => e.message === "bid accepted").length, 1);

        // A fresh load starts the record over. What is left is the harness minting this
        // test's own session, which the framework records like any other session: an
        // assertion of an empty list would be asserting that the framework stopped saying
        // things it is supposed to say.
        verify(harness.load(), harness.errorString);
        compare(harness.recorded().filter(e => e.categoryName === "application").length, 0);
    }

    // Through the contract. `call()` goes through the generated slot, so the `<admin>` gate
    // on clearBids and the `string[8]` bound on note refuse here as they do on the wire.
    function test_a_gated_slot_refuses_a_caller_below_its_scope() {
        harness.callerIsUser("moderator", { sub: "mod" });
        ignoreWarning(/Ledger\.clearBids: refused, the caller does not hold the scope/);
        harness.call("clearBids");
        compare(harness.subject.highBid, 100);
    }

    function test_a_gated_slot_answers_a_caller_in_its_scope() {
        harness.callerIsUser("admin", { sub: "root" });
        harness.call("clearBids");
        compare(harness.subject.highBid, 0);
    }

    function test_an_argument_past_its_bound_never_reaches_the_function() {
        harness.callerIsUser("user", { sub: "alice" });
        ignoreWarning(/Ledger\.note/);
        harness.call("note", ["far longer than eight"]);
        compare(harness.subject.lastNote, "");
        harness.call("note", ["short"]);
        compare(harness.subject.lastNote, "short");
    }

    function test_a_returning_slot_answers_through_the_contract() {
        harness.callerIsEntity("web");
        compare(harness.call("recordWinner", ["vase", "alice", 120]), true);
        harness.callerIsEntity("rogue");
        compare(harness.call("recordWinner", ["vase", "mallory", 1]), false);
    }

    // A gated prop publishes what the owner assigned only to a caller in its scope.
    function test_a_gated_prop_is_published_to_a_caller_in_its_scope_only() {
        harness.callerIsUser("user", { sub: "alice" });
        compare(harness.subject.auditNote, "");
        harness.callerIsUser("moderator", { sub: "mod" });
        compare(harness.subject.auditNote, "two bids withdrawn");
    }
}
