# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Unit tests for the synqtc contract generator.

Run with: python3 -m unittest discover -s tools/synqtc
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from synqtc import SynError, parse_text  # noqa: E402
from synqtc.emit import (  # noqa: E402
    emit_consumer_header,
    emit_consumer_source,
    emit_rep,
    emit_source_helper_header,
    emit_source_helper_source,
)

TODO = """
contract Todo {
    prop int count
    model items(string text, string author, bool done)
    slot add(string text)
    slot bool clear()
    signal rejected(string reason)
}
"""


class RepLoweringTest(unittest.TestCase):
    def rep(self, text):
        return emit_rep(parse_text(text, stem="Todo"))

    def test_prop_is_readpush_never_readwrite(self):
        rep = self.rep(TODO)
        self.assertIn("PROP(int count READPUSH)", rep)
        self.assertNotIn("READWRITE", rep)

    def test_model_limited_to_declared_roles(self):
        rep = self.rep(TODO)
        self.assertIn("MODEL items(text, author, done)", rep)

    def test_fire_and_forget_slot_is_void(self):
        self.assertIn("SLOT(void add(QString text))", self.rep(TODO))

    def test_returning_slot_keeps_return_type(self):
        self.assertIn("SLOT(bool clear())", self.rep(TODO))

    def test_signal_lowers(self):
        self.assertIn("SIGNAL(rejected(QString reason))", self.rep(TODO))

    def test_record_lowers_to_pod(self):
        rep = self.rep("record Address(string street, int zip)")
        self.assertIn("POD Address(QString street, int zip)", rep)

    def test_type_mapping(self):
        rep = self.rep(
            "contract T { prop real r prop var v prop bool b prop double d }"
        )
        for expected in (
            "PROP(double r READPUSH)",
            "PROP(QVariant v READPUSH)",
            "PROP(bool b READPUSH)",
            "PROP(double d READPUSH)",
        ):
            self.assertIn(expected, rep)

    def test_record_used_as_param_type(self):
        rep = self.rep(
            "record ItemRow(string text) contract Items { slot insert(ItemRow row) }"
        )
        self.assertIn("POD ItemRow(QString text)", rep)
        self.assertIn("SLOT(void insert(ItemRow row))", rep)


class ModelRoleTypeTest(unittest.TestCase):
    """A role is typed; `var` is allowed for a role that carries anything."""

    def roles(self, text):
        return parse_text(text, stem="C").contracts[0].models[0].roles

    def test_a_model_role_carries_its_type(self):
        roles = self.roles("contract C { model rows(string id, int count) }")
        self.assertEqual([(role.type, role.name) for role in roles],
                         [("string", "id"), ("int", "count")])

    def test_var_is_a_legal_role_type(self):
        roles = self.roles("contract C { model rows(string id, var payload) }")
        self.assertEqual(roles[1].type, "var")

    def test_a_record_is_a_legal_role_type(self):
        roles = self.roles("record Bid(int amount) contract C { model rows(Bid top) }")
        self.assertEqual(roles[0].type, "Bid")

    def test_an_untyped_role_is_a_parse_error_naming_the_line(self):
        with self.assertRaises(SynError) as caught:
            parse_text("contract C {\n    model rows(id, count)\n}", path="bad.syn")
        self.assertEqual(caught.exception.line, 2)
        self.assertIn("role", caught.exception.message)

    def test_an_unknown_role_type_is_refused(self):
        with self.assertRaises(SynError) as caught:
            parse_text("contract C { model rows(widget id) }", path="bad.syn")
        self.assertIn("widget", caught.exception.message)

    def test_the_rep_model_line_still_carries_names_only(self):
        # repc's roles are names. The types are the generator's to enforce at the boundary.
        rep = emit_rep(parse_text("contract C { model rows(string id, int n) }", stem="C"))
        self.assertIn("MODEL rows(id, n)", rep)

    def test_the_helper_converts_each_declared_role(self):
        syn = parse_text("contract C { model rows(string id, int n) }", stem="C")
        source = emit_source_helper_source(syn, "c")
        self.assertIn("QMetaType::fromType<QString>()", source)
        self.assertIn("QMetaType::fromType<int>()", source)

    def test_a_var_role_is_stored_as_it_arrives(self):
        syn = parse_text("contract C { model rows(var payload) }", stem="C")
        source = emit_source_helper_source(syn, "c")
        self.assertNotIn("QMetaType::fromType<QVariant>()", source)

    def test_the_published_model_is_the_one_a_consumer_cannot_write(self):
        syn = parse_text("contract C { model rows(string id) }", stem="C")
        self.assertIn("SynQt::SourceModel m_rowsModel;", emit_source_helper_header(syn, "c"))


class SourceHelperTest(unittest.TestCase):
    def test_helper_exposes_set_model_and_registers_qml_type(self):
        syn = parse_text(TODO, stem="Todo")
        header = emit_source_helper_header(syn, "todo")
        source = emit_source_helper_source(syn, "todo")
        self.assertIn("class TodoSourceHelper : public TodoSimpleSource", header)
        self.assertIn("Q_INVOKABLE void setItems(const QVariantList &rows)", header)
        # The declared roles, and nothing else, drive the published model.
        self.assertIn('QByteArrayLiteral("text")', source)
        self.assertIn('QByteArrayLiteral("author")', source)
        self.assertIn('QByteArrayLiteral("done")', source)
        # Registered under the contract's own name; an entity never consumes a contract it
        # owns.
        self.assertIn('qmlRegisterType<TodoSourceHelper>("SynQt", 1, 0, "Todo")', source)

    def test_registering_sources_installs_the_qtquick_re_export(self):
        # Registering the contract installs the QtQuick re-export (see
        # tests/m1-contract/tst_qmlimport.cpp).
        syn = parse_text(TODO, stem="Todo")
        source = emit_source_helper_source(syn, "todo")
        self.assertIn("#if __has_include(<moduleimports.h>)", source)
        self.assertIn("SynQt::registerModuleImports();", source)
        # Inside the registration, not at file scope where nothing would run it.
        registration = source.split("void synqtRegisterTodoSources()", 1)[1]
        self.assertIn("SynQt::registerModuleImports();", registration)

    def test_slots_are_concrete_overrides(self):
        syn = parse_text(TODO, stem="Todo")
        header = emit_source_helper_header(syn, "todo")
        self.assertIn("void add(QString text) override;", header)
        self.assertIn("bool clear() override;", header)

    def test_contract_with_signals_emits_typed_caller_sugar(self):
        # A contract with a signal gets a <Contract>Caller with emit<Signal>(...), guarded
        # on caller.h, and a registered factory.
        syn = parse_text(TODO, stem="Todo")
        header = emit_source_helper_header(syn, "todo")
        source = emit_source_helper_source(syn, "todo")
        self.assertIn("#if __has_include(<caller.h>)", header)
        self.assertIn("class TodoCaller : public SynQt::Caller", header)
        self.assertIn("Q_INVOKABLE void emitRejected(QString reason)", header)
        self.assertIn('emitSignal(QStringLiteral("rejected"), QVariant::fromValue(reason))',
                      header)
        self.assertIn("#if __has_include(<caller.h>)", source)
        self.assertIn('SynQt::Caller::registerCallerFactory(QStringLiteral("Todo")', source)

    def test_contract_without_signals_has_no_caller_subclass(self):
        # A signal-less contract gets no <Contract>Caller and no factory.
        syn = parse_text("contract Catalog { prop int count slot pick(int index) }",
                         stem="Catalog")
        header = emit_source_helper_header(syn, "catalog")
        source = emit_source_helper_source(syn, "catalog")
        self.assertNotIn("CatalogCaller", header)
        self.assertNotIn("registerCallerFactory", source)


class ConsumerFacadeTest(unittest.TestCase):
    def header(self, text=TODO, stem="Todo", lstem="todo"):
        return emit_consumer_header(parse_text(text, stem=stem), lstem)

    def source(self, text=TODO, stem="Todo", lstem="todo"):
        return emit_consumer_source(parse_text(text, stem=stem), lstem)

    def test_facade_forwards_property_model_and_slots(self):
        # The facade exposes push properties and models as Q_PROPERTY, void slots as
        # Q_INVOKABLE, and returning slots as Promise-returning ones.
        header = self.header()
        self.assertIn("class TodoConsumer : public SynQt::ConsumerBase", header)
        self.assertIn("Q_PROPERTY(int count READ count NOTIFY countChanged)", header)
        self.assertIn("Q_PROPERTY(QAbstractItemModel *items READ items NOTIFY itemsChanged)",
                      header)
        self.assertIn("Q_INVOKABLE void add(QString text);", header)
        self.assertIn("Q_INVOKABLE SynQt::Promise *clear();", header)

    def test_returning_slot_resolves_a_promise_via_pending_reply(self):
        # A returning slot invokes the Replica and wraps the typed pending reply in a Promise.
        source = self.source()
        self.assertIn("SynQt::Promise *TodoConsumer::clear()", source)
        self.assertIn("QRemoteObjectPendingReply<bool> reply;", source)
        self.assertIn("return new SynQt::Promise{reply, engine, this};", source)

    def test_one_qml_name_is_the_facade_itself(self):
        # `Todo.add(...)`, `Todo.count` and `Todo.onRejected:` are one object: the attaching
        # function returns the live facade.
        header = self.header()
        source = self.source()
        self.assertIn("QML_ATTACHED(TodoConsumer)", header)
        self.assertIn("static TodoConsumer *qmlAttachedProperties(QObject *object);", header)
        self.assertIn("void rejected(QString reason);", header)  # the contract's own signal
        self.assertNotIn("class TodoAttached", header)
        self.assertIn("TodoConsumer *TodoConsumer::qmlAttachedProperties(QObject *object)",
                      source)
        self.assertIn('ConnectPointResolver::instance()->resolve(QStringLiteral("Todo"))',
                      source)
        self.assertIn("QQmlEngine::setObjectOwnership(facade, QQmlEngine::CppOwnership);",
                      source)
        self.assertIn('qmlRegisterType<TodoConsumer>("SynQt", 1, 0, "Todo");', source)
        self.assertIn('SynQt::registerConsumerFactory(QStringLiteral("Todo")', source)
        # The consumer side installs the QtQuick re-export too.
        registration = source.split("void synqtRegisterTodoConsumers()", 1)[1]
        self.assertIn("SynQt::registerModuleImports();", registration)

    def test_everything_is_guarded_on_the_runtime_header(self):
        # A Replica-only target compiles to the registration stub.
        header = self.header()
        source = self.source()
        self.assertIn("#if __has_include(<consumerbase.h>)", header)
        self.assertIn("void synqtRegisterTodoConsumers();", header)
        self.assertIn("#else\nvoid synqtRegisterTodoConsumers() {}", source)


class MalformedInputTest(unittest.TestCase):
    CASES = {
        "unknown top-level keyword": "widget Foo { }",
        "prop missing name": "contract C { prop int }",
        "unknown type": "contract C { prop money amount }",
        "empty model roles": "contract C { model items() }",
        "unterminated contract": "contract C { prop int x ",
        "reserved word as name": "contract C { prop int slot }",
        "duplicate contract": "contract C {} contract C {}",
        "slot with two type words": "contract C { slot int foo bar() }",
        "unterminated comment": "contract C { /* nope }",
        "stray character": "contract C { prop int x @ }",
        "bound of zero": "contract C { prop string[0] name }",
        "bound on a type that has none": "contract C { prop int[4] tally }",
        "bound with no number": "contract C { prop string[] name }",
        "bound on a slot name": "contract C { slot post[2](string text) }",
        "a width that QML does not spell": "contract C { prop int16 tally }",
        "void as a declared type": "contract C { slot void post(string text) }",
    }

    def test_each_malformed_input_raises_synerror_with_location(self):
        for label, text in self.CASES.items():
            with self.subTest(label=label):
                with self.assertRaises(SynError) as ctx:
                    parse_text(text, path="bad.syn")
                message = ctx.exception.format()
                self.assertIn("error:", message)
                self.assertTrue(message.startswith("bad.syn"))


class ValueTypeTest(unittest.TestCase):
    """The vocabulary is QML's own value types, lowered to the C++ each one means."""

    SYN = """
        contract Every {
            prop bool live
            prop date opened
            prop double ratio
            prop int tally
            prop list entries
            prop real share
            prop string title
            prop url home
            prop var payload
            prop variant legacy
        }
    """

    def setUp(self):
        self.syn = parse_text(self.SYN, path="every.syn", stem="every")
        self.rep = emit_rep(self.syn)

    def test_each_value_type_lowers_to_its_qt_spelling(self):
        expected = {
            "live": "bool", "opened": "QDateTime", "ratio": "double", "tally": "int",
            "entries": "QVariantList", "share": "double", "title": "QString",
            "home": "QUrl", "payload": "QVariant", "legacy": "QVariant",
        }
        for name, ctype in expected.items():
            with self.subTest(prop=name):
                self.assertIn(f"PROP({ctype} {name} READPUSH)", self.rep)

    def test_the_rep_includes_the_headers_its_types_need(self):
        # repc's own includes stop at qvariant.h, so the needed headers are emitted.
        for header in ("QDateTime", "QUrl", "QString", "QVariantList", "QVariant"):
            with self.subTest(header=header):
                self.assertIn(f"#include <{header}>", self.rep)


class BoundedTypeTest(unittest.TestCase):
    """The boundary enforces every bound written in a contract."""

    SYN = """
        contract Players {
            prop string[16] region
            prop list[8] recent
            model rows(string[64] playerId, url[200] avatar, var[4096] extra)
            slot lookup(string[64] playerId, var[512] filter)
            signal refused(string[32] reason)
        }
    """

    def setUp(self):
        self.syn = parse_text(self.SYN, path="players.syn", stem="players")
        self.header = emit_source_helper_header(self.syn, "players")
        self.source = emit_source_helper_source(self.syn, "players")

    def test_a_bounded_prop_refuses_a_value_that_does_not_fit(self):
        # repc makes every setter virtual, so overriding it is the whole interception.
        self.assertIn("void setRegion(QString region) override;", self.header)
        self.assertIn("const qsizetype regionSize{region.size()};", self.source)
        self.assertIn("if (regionSize > 16) {", self.source)

    def test_a_bounded_list_counts_its_elements(self):
        self.assertIn("const qsizetype recentSize{recent.size()};", self.source)
        self.assertIn("is declared %s and the value is %lld elements", self.source)
        self.assertIn('"Players.recent", "recent", "list[8]"', self.source)

    def test_a_bounded_role_refuses_the_publish(self):
        self.assertIn("if (playerIdSize > 64) {", self.source)
        self.assertIn("qDeleteAll(items);", self.source)

    def test_a_bounded_url_is_measured_as_the_text_it_is(self):
        self.assertIn("qvariant_cast<QUrl>(avatarValue).toString().size()", self.source)

    def test_a_bounded_var_is_measured_by_what_it_serializes_to(self):
        self.assertIn("qsizetype synqtVariantBytes(const QVariant &value)", self.source)
        self.assertIn("const qsizetype extraSize{synqtVariantBytes(extraValue)};",
                      self.source)
        self.assertIn("is declared %s and the value is %lld bytes", self.source)
        self.assertIn('"Players.rows", "extra", "var[4096]"', self.source)

    def test_an_unbounded_var_role_carries_whatever_arrives(self):
        plain = parse_text("contract P { model rows(var extra) }", path="p.syn", stem="p")
        source = emit_source_helper_source(plain, "p")
        self.assertIn("extra: declared var", source)
        self.assertNotIn("synqtVariantBytes", source)

    def test_a_bounded_slot_argument_is_refused_before_the_owners_qml_sees_it(self):
        slot = self.source[self.source.index("void PlayersSourceHelper::lookup"):]
        refusal = slot.index("playerIdSize > 64")
        dispatch = slot.index("synqtQmlSlotIndex")
        self.assertLess(refusal, dispatch)

    def test_a_bounded_signal_argument_is_refused_on_the_way_out(self):
        # Caller.emit<Signal> forwards here too, so one guard covers both ways of sending.
        emitter = self.source[self.source.index("void PlayersSourceHelper::emitRefused"):]
        refusal = emitter.index("reasonSize > 32")
        raised = emitter.index("Q_EMIT refused(")
        self.assertLess(refusal, raised)


class ForwardedSessionTest(unittest.TestCase):
    """A contract a service consumes carries the session the calling entity acts for."""

    SYN = """
        contract Ledger {
            prop int count
            slot note(string item)
            slot bool clear()
            signal noted(string item)
        }
    """

    def parse(self, forwards: bool):
        syn = parse_text(self.SYN, path="ledger.syn", stem="ledger")
        syn.forwards_session = forwards
        return syn

    def test_a_forwarding_contract_carries_the_session_on_every_slot(self):
        rep = emit_rep(self.parse(True))
        self.assertIn("SLOT(void note(QVariantMap synqtSession, QString item))", rep)
        self.assertIn("SLOT(bool clear(QVariantMap synqtSession))", rep)
        # Owner to consumer, so there is nobody to be acting for on the way out.
        self.assertIn("SIGNAL(noted(QString item))", rep)

    def test_a_browser_only_contract_has_no_field_to_forge(self):
        rep = emit_rep(self.parse(False))
        self.assertIn("SLOT(void note(QString item))", rep)
        self.assertNotIn("synqtSession", rep)

    def test_the_owner_takes_the_session_before_it_does_anything_else(self):
        source = emit_source_helper_source(self.parse(True), "ledger")
        slot = source[source.index("void LedgerSourceHelper::note"):]
        taken = slot.index('"assumeSession"')
        dispatch = slot.index("synqtQmlSlotIndex")
        self.assertLess(taken, dispatch)

    def test_the_span_opens_after_the_session_it_continues_is_taken(self):
        # The session is taken before the span opens, so the span continues the right trace.
        source = emit_source_helper_source(self.parse(True), "ledger")
        slot = source[source.index("void LedgerSourceHelper::note"):]
        taken = slot.index('"assumeSession"')
        opened = slot.index("SynqtCallSpan synqtSpan{")
        self.assertLess(taken, opened)

    def test_the_owner_does_not_hand_the_session_to_the_owners_qml(self):
        # The QML implementation takes the arguments the contract declares and no others.
        source = emit_source_helper_source(self.parse(True), "ledger")
        slot = source[source.index("void LedgerSourceHelper::note"):]
        self.assertIn('synqtQmlSlotIndex(this, "note", 1,', slot)
        self.assertNotIn("QVariant::fromValue(synqtSession)", slot)

    def test_every_slot_names_the_caller_it_is_answering(self):
        # Also on a browser-facing point, where a chain starts.
        for forwards in (True, False):
            with self.subTest(forwards=forwards):
                source = emit_source_helper_source(self.parse(forwards), "ledger")
                self.assertIn("const SynqtActingFor synqtActing{m_synqtCaller.data()};",
                              source)

    def test_the_consumer_fills_the_session_in_rather_than_the_call_site(self):
        consumer = emit_consumer_source(self.parse(True), "ledger")
        self.assertIn("void LedgerConsumer::note(QString item)", consumer)
        self.assertIn("Q_ARG(QVariantMap, SynQt::ActingFor::current()), Q_ARG(QString, item)",
                      consumer)


class SharedSourceTest(unittest.TestCase):
    """A shared entity answers everyone from one Source, through a mirror per caller."""

    SYN = """
        contract Board {
            prop string topic
            model notes(string author)
            slot post(string text)
            signal rejected(string reason)
        }
    """

    def setUp(self):
        self.syn = parse_text(self.SYN, path="board.syn", stem="board")
        self.header = emit_source_helper_header(self.syn, "board")
        self.source = emit_source_helper_source(self.syn, "board")

    def test_the_runtime_reaches_both_hooks_by_name(self):
        # It knows the contract's name and nothing about its type, so both are invokable.
        self.assertIn("Q_INVOKABLE void synqtSetCaller(QObject *caller);", self.header)
        self.assertIn("Q_INVOKABLE void synqtMirror(QObject *shared);", self.header)

    def test_a_mirror_follows_every_pushed_thing(self):
        self.assertIn("setTopic(source->topic());", self.source)
        self.assertIn("&BoardSource::topicChanged", self.source)
        self.assertIn("setNotes(source->notesRows());", self.source)
        self.assertIn("&BoardSource::rejected", self.source)

    def test_a_slot_forwards_with_the_caller_bound(self):
        post = self.source[self.source.index("void BoardSourceHelper::post"):]
        self.assertIn("m_synqtShared->synqtAdoptCaller(m_synqtCaller);", post)
        self.assertIn("m_synqtShared->post(text);", post)

    def test_the_type_registers_a_way_to_build_one(self):
        self.assertIn("SynQt::SourceFactory::registerSource(QStringLiteral(\"Board\")",
                      self.source)


class CallSpanTest(unittest.TestCase):
    """Every slot crossing a link is timed, and the record names the check that refused it. The
    span is a declaration closed by its destructor, behind `__has_include`.
    """

    SYN = """
        contract Hall {
            slot enter(string[32] name)
            <moderator> slot promote(string[32] name)
            slot int count()
        }
    """

    def setUp(self):
        self.syn = parse_text(self.SYN, path="hall.syn", stem="hall")
        self.source = emit_source_helper_source(self.syn, "hall")

    def test_every_slot_opens_a_span_naming_its_contract_and_member(self):
        self.assertIn('SynqtCallSpan synqtSpan{"Hall", "enter",', self.source)
        self.assertIn('SynqtCallSpan synqtSpan{"Hall", "promote",', self.source)
        # A returning slot too: a call that answers is still a call that crossed a link.
        self.assertIn('SynqtCallSpan synqtSpan{"Hall", "count",', self.source)

    def test_the_span_is_told_how_many_arguments_there_were_and_never_their_values(self):
        self.assertIn("m_synqtCaller.data(), 1};", self.source)
        self.assertIn("m_synqtCaller.data(), 0};", self.source)
        # Only the count reaches the span. Nothing hands it an argument.
        self.assertNotIn("synqtSpan.capture(", self.source)

    def test_a_member_that_asks_for_its_values_gets_them_and_the_others_do_not(self):
        asked = parse_text("contract Hall { slot capture enter(string[32] name) "
                           "slot promote(string[32] name) }",
                           path="hall.syn", stem="hall")
        source = emit_source_helper_source(asked, "hall")
        self.assertIn('synqtSpan.capture(QStringLiteral("name"),', source)
        # One member asking says nothing about the next one: the flag is per member.
        self.assertEqual(source.count("synqtSpan.capture("), 1)

    def test_a_captured_value_is_taken_after_its_bound_is_checked(self):
        asked = parse_text("contract Hall { slot capture enter(string[32] name) }",
                           path="hall.syn", stem="hall")
        source = emit_source_helper_source(asked, "hall")
        # Capture runs after the bounds, so an oversized argument is not recorded.
        self.assertLess(source.index("if (nameSize > 32) {"),
                        source.index("synqtSpan.capture("))

    def test_capture_is_not_a_reserved_word(self):
        # A slot may still be called `capture`, which one it is settles on the token after.
        named = parse_text("contract Hall { slot capture() }", path="hall.syn", stem="hall")
        self.assertEqual(named.contracts[0].slots[0].name, "capture")
        self.assertFalse(named.contracts[0].slots[0].capture)

    def test_a_refusal_names_the_check_that_made_it(self):
        self.assertIn('synqtSpan.refuse("scope");', self.source)
        self.assertIn('synqtSpan.refuse("bound");', self.source)

    def test_a_contract_only_target_compiles_against_a_span_that_does_nothing(self):
        self.assertIn("#if __has_include(<callspan.h>)", self.source)
        self.assertIn("using SynqtCallSpan = SynQt::CallSpan;", self.source)
        self.assertIn("SynqtCallSpan(const char *, const char *, QObject *, int) {}",
                      self.source)

    def test_a_contract_with_no_slots_carries_none_of_it(self):
        quiet = parse_text("contract Q { prop int value }", path="q.syn", stem="q")
        source = emit_source_helper_source(quiet, "q")
        self.assertNotIn("SynqtCallSpan", source)


if __name__ == "__main__":
    unittest.main()
