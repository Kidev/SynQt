<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The QML test harness

What an application gets from `synqt test`: a connect point's Source, loaded on its own,
with a caller the test chooses, driven from QML. This suite is the framework's own copy of
that arrangement, so the harness breaks here before it breaks in someone's app.

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/entity-test/run-entitytest.sh
```

## What is under test

[`web/Ledger.qml`](web/Ledger.qml) is an ordinary owner implementation. It is written
exactly as an application would write one and knows nothing about a harness, which is the
condition that makes the suite mean anything: nothing in it may be adjusted to make a test
pass. It authorizes a user by scope, refuses a bid that does not beat the standing one,
answers each refusal to the one caller, and gates its permanent record on the calling
entity.

[`qml/tst_ledger.qml`](qml/tst_ledger.qml) is the test, and it is the same file an
application author writes: `TestCase`, `SignalSpy`, and one `EntityTest`. There is no C++
in it and no database, server or certificate behind it.

[`tst_entitytest.cpp`](tst_entitytest.cpp) is the runner, and it is deliberately two
registrations and nothing else. `synqt test` generates the same file for an application
(`cmakegen.render_tests_cmakelists` and `maingen.render_tests_main`), so if this one grows
logic, the generated one is wrong.

## Why the Caller is the real one

The harness mints `Caller` through `Caller::forUser` and `Caller::forEntity`, the same
factories the web edge and the mesh transports call. There is no test-only constructor and
no way to set a scope that the runtime does not also have, so a slot cannot pass here and
fail in production because the test stubbed the check. The scope order is the one a
scaffolded project gets, hierarchical, so `hasScope("user")` is satisfied by a moderator
here exactly as it is on a running edge.

What the harness substitutes is only the engine behind a blueprint helper: `Db` on SQLite
in memory, `Cache` and `Docs` on the memory providers. `test_each_test_starts_from_an_empty_database`
pins the part that would otherwise rot silently, that `load()` really does reset it.

The harness ships in `SynQtTesting`, a library a production entity never links, and
registers into `SynQt.Test`, an import a production entity never writes.
