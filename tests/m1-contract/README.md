<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The contract generator

Turns this suite's `shared/*.syn` contracts, which is the form a connect point's
`export:` block compiles to, into the QtRemoteObjects layer: a `.rep` driven through
repc, the owner-side Source helper, and the consumer-side Replica QML registration. The
`.syn` surface keeps QtRO's safe defaults visible, so a mistake cannot silently become a
security hole.

The generator itself lives at [`tools/synqtc/`](../../tools/synqtc), and the CMake glue is
[`cmake/SynQtContracts.cmake`](../../cmake/SynQtContracts.cmake). This directory is the
acceptance test.

## Verdict

**PASS.** Every acceptance criterion, verified:

| criterion | evidence |
|-----------|----------|
| compiling Source and Replica headers | `tst_m1`, `m1_owner`, `m1_client` all build and link |
| `prop` -> `PROP(... READPUSH)`, never READWRITE | `repHasSafeDefaults` |
| `model` -> `MODEL name(roles)` limited to declared roles | `repHasSafeDefaults` |
| `signal`/`slot`, slot return type when present | `repHasSafeDefaults`, `roundTripAllDirections` |
| `record` -> `POD` | `repHasSafeDefaults` (Catalog's `ItemRow`) |
| Source helper `set<Model>(rows)` keeps declared roles, drops undeclared fields | `setModelDropsUndeclaredRoles` |
| no `<model>Changed` push, no consumer write path | READPUSH default + owner-only `set<Model>` |
| QML registrations (Replica for consumers, Source helper for owners) | `qmlRegistrationsAreEmitted` |
| malformed input rejected clearly | 10 cases in `test_synqtc.py`, and step 2 of `run-m1.sh` |
| owner/consumer split (Source helper never in the client) | `m1_client` links without Qt Gui; `m1_owner` links it |

## Lowering rules

| `.syn` | rep | notes |
|--------|-----|-------|
| `prop int count` | `PROP(int count READPUSH)` | READPUSH is the default, never READWRITE |
| `model items(text, author, done)` | `MODEL items(text, author, done)` | only declared roles cross |
| `slot add(string text)` | `SLOT(void add(QString text))` | fire and forget |
| `slot bool clear()` | `SLOT(bool clear())` | returning slot -> async call on the consumer |
| `signal rejected(string reason)` | `SIGNAL(rejected(QString reason))` | owner -> consumer |
| `record ItemRow(string text, ...)` | `POD ItemRow(QString text, ...)` | passed by value |

Types are QML's built-in value types: `bool`->`bool`, `date`->`QDateTime`,
`int`->`int`, `list`->`QVariantList`, `real`/`double`->`double`, `string`->`QString`,
`url`->`QUrl`, `var`/`variant`->`QVariant`, and a record name to its POD. Four of them
take a bound: `string[n]` and `url[n]` in characters, `list[n]` in elements, and `var[n]`
in serialized bytes. The owner-side boundary refuses a value over its bound, and never
truncates it.

## The owner surface

For each `model`, the generated Source helper (`<Contract>Source` in QML, `import
SynQt`) exposes `set<Model>(rows)`. It takes an array of row objects as the new
authoritative model state, keeps only the declared roles, and drops any undeclared
owner-only field at the boundary. There is no `<model>Changed` push and no consumer write
path, because properties are READPUSH and the model setter is owner-only.

## The QML rules the build is written against

`tst_qmlrules` is not a contract-generator criterion. It lives here because it is about
how a file gets its type. `synqt build` mirrors every entity's QML into `generated/` and
retypes a root object named after its own file
([`synqt/qmlrewrite.py`](../../tools/synqt/synqt/qmlrewrite.py)). That is a decision taken
in Python about how a QML engine will behave, and Python cannot check it, so the three
facts it rests on are checked here against a real engine:

| fact | why the tooling needs it |
|------|--------------------------|
| a root object named after its own file, with no type of that name in scope, is refused ("instantiated recursively") | why the mirror exists at all |
| the same file loads when an import does provide that type, because an explicit import beats the containing directory's implicit one | why a connect point's Source, rooted at its contract, is copied unchanged |
| `import SynQt` brings QtQuick with it (`SynQt::registerModuleImports`) | why an entity's file needs one import line rather than two |

If Qt ever changes one of these, the tooling is wrong in a way nothing else would name.
The generated tree would still be written, and entities would fail to load at start-up.

## How to run

```sh
tests/m1-contract/run-m1.sh
```

It runs the generator's Python unit tests, checks that a malformed contract is
refused, builds the targets, and runs the QtRO round-trip acceptance test. Or directly:

```sh
python3 -m unittest tests.test_synqtc          # from tools/synqtc/
cmake -S tests/m1-contract -B build/m1-contract -G Ninja \
  -DCMAKE_PREFIX_PATH=/opt/Qt/6.12.0/gcc_64 && cmake --build build/m1-contract
ctest --test-dir build/m1-contract --output-on-failure
```

## Notes / findings

- repc and PODs across roles. A rep containing a POD defines its `Q_GADGET` in both the
  `_source.h` and the `_replica.h`, so a target that is both an owner and a consumer has
  to use repc's merged header, which emits the POD once. Real entities are owner-only
  (`ROLE source`) or consumer-only (`ROLE replica`) and never reach this. Only the
  both-sided `tst_m1` uses `ROLE both`, which `synqt_add_contract` maps to
  `qt_add_repc_merged`. The Source helper and Replica sources include a stable
  `<stem>_rep.h` indirection that the build points at the repc header for the role.
- Slots on the generated helper are concrete no-ops, so the QML type is instantiable.
  The entity runtime wires the dispatch of a consumer slot call into the owner's QML
  implementation, with the `Caller` accessor.
