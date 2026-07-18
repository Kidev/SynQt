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
    model items(text, author, done)
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
            "contract T { prop real r prop var v prop bool b prop float f prop double d }"
        )
        for expected in (
            "PROP(double r READPUSH)",
            "PROP(QVariant v READPUSH)",
            "PROP(bool b READPUSH)",
            "PROP(float f READPUSH)",
            "PROP(double d READPUSH)",
        ):
            self.assertIn(expected, rep)

    def test_record_used_as_param_type(self):
        rep = self.rep(
            "record ItemRow(string text) contract Items { slot insert(ItemRow row) }"
        )
        self.assertIn("POD ItemRow(QString text)", rep)
        self.assertIn("SLOT(void insert(ItemRow row))", rep)


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
        self.assertIn('qmlRegisterType<TodoSourceHelper>("SynQt", 1, 0, "TodoSource")', source)

    def test_slots_are_concrete_overrides(self):
        syn = parse_text(TODO, stem="Todo")
        header = emit_source_helper_header(syn, "todo")
        self.assertIn("void add(QString text) override;", header)
        self.assertIn("bool clear() override;", header)

    def test_contract_with_signals_emits_typed_caller_sugar(self):
        # A contract with a signal gets a <Contract>Caller whose emit<Signal>(...) forwards
        # to emitSignal, giving owner QML Caller.emit<Signal>(...). It is guarded on the
        # service runtime's caller.h so a rep-only target still compiles, and a factory is
        # registered so forUser/forEntity mint it.
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
        # A signal-less contract gets no <Contract>Caller and no factory (the base Caller,
        # with emitSignal, remains available).
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
        # The <Contract>Consumer facade exposes each push property and model as a Q_PROPERTY,
        # a void slot as a plain Q_INVOKABLE, and a returning slot as a Promise-returning one.
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

    def test_attached_type_registered_under_the_contract_name(self):
        # `<Contract>.on<Signal>` needs an attached type named after the contract, resolving
        # the consumed connect point through the resolver and relaying its signals.
        header = self.header()
        source = self.source()
        self.assertIn("class TodoAttached : public QObject", header)
        self.assertIn("QML_ATTACHED(TodoAttached)", header)
        self.assertIn("void rejected(QString reason);", header)  # relayed contract signal
        self.assertIn('qmlRegisterType<Todo>("SynQt", 1, 0, "Todo");', source)
        self.assertIn('SynQt::registerConsumerFactory(QStringLiteral("Todo")', source)

    def test_everything_is_guarded_on_the_runtime_header(self):
        # A Replica-only target (no consumer runtime) compiles the file away to just the
        # registration stub, so it still links.
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
    }

    def test_each_malformed_input_raises_synerror_with_location(self):
        for label, text in self.CASES.items():
            with self.subTest(label=label):
                with self.assertRaises(SynError) as ctx:
                    parse_text(text, path="bad.syn")
                message = ctx.exception.format()
                self.assertIn("error:", message)
                self.assertTrue(message.startswith("bad.syn"))


if __name__ == "__main__":
    unittest.main()
