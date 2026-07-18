<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M1: Contract generator

Turns `shared/*.syn` contracts into the QtRemoteObjects layer: a `.rep` (driven
through repc), the owner-side Source helper, and the consumer-side Replica QML
registration. The `.syn` surface keeps QtRO's safe defaults obvious so a mistake
cannot silently become a security hole.

The generator itself lives at [`tools/synqtc/`](../../tools/synqtc); the CMake glue is
[`cmake/SynQtContracts.cmake`](../../cmake/SynQtContracts.cmake). This directory is the
acceptance test.

## Verdict

**PASS.** Acceptance criteria from the M1 build guide, all verified:

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
| malformed input rejected clearly | 10 cases in `test_synqtc.py`; `run-m1.sh` step 2 |
| owner/consumer split (Source helper never in the client) | `m1_client` links without Qt Gui; `m1_owner` links it |

## Lowering rules

| `.syn` | rep | notes |
|--------|-----|-------|
| `prop int count` | `PROP(int count READPUSH)` | READPUSH is the default; never READWRITE |
| `model items(text, author, done)` | `MODEL items(text, author, done)` | only declared roles cross |
| `slot add(string text)` | `SLOT(void add(QString text))` | fire and forget |
| `slot bool clear()` | `SLOT(bool clear())` | returning slot -> async call on the consumer |
| `signal rejected(string reason)` | `SIGNAL(rejected(QString reason))` | owner -> consumer |
| `record ItemRow(string text, ...)` | `POD ItemRow(QString text, ...)` | passed by value |

Types: `int`->`int`, `string`->`QString`, `bool`->`bool`, `real`/`double`->`double`,
`float`->`float`, `var`->`QVariant`, a record name -> its POD.

## The owner surface

For each `model`, the generated Source helper (`<Contract>Source` in QML, `import
SynQt`) exposes `set<Model>(rows)`: it takes an array of row objects as the new
authoritative model state, keeps only the declared roles, and drops any undeclared
owner-only field at the boundary. There is no `<model>Changed` push and no consumer
write path; properties are READPUSH and the model setter is owner-only.

## How to run

```sh
tests/m1-contract/run-m1.sh
```

Runs the generator's Python unit tests, checks a malformed contract is rejected,
builds the three targets, and runs the QtRO round-trip acceptance test. Or directly:

```sh
python3 -m unittest tests.test_synqtc          # from tools/synqtc/
cmake -S tests/m1-contract -B build/m1-contract -G Ninja \
  -DCMAKE_PREFIX_PATH=/opt/Qt/6.11.1/gcc_64 && cmake --build build/m1-contract
ctest --test-dir build/m1-contract --output-on-failure
```

## Notes / findings

- **repc + PODs across roles.** A rep containing a POD defines its `Q_GADGET` in both
  the `_source.h` and `_replica.h`; a target that is both an owner and a consumer must
  therefore use repc's *merged* header (POD emitted once). Real entities are owner-only
  (`ROLE source`) or consumer-only (`ROLE replica`) and never hit this; only the
  both-sided `tst_m1` uses `ROLE both`, which `synqt_add_contract` maps to
  `qt_add_repc_merged`. The Source helper and Replica sources include a stable
  `<stem>_rep.h` indirection the build points at the repc header for the role.
- Slots on the generated helper are concrete no-ops so the QML type is instantiable;
  dispatching a consumer slot call into the owner's QML implementation (with the
  `Caller` accessor) is wired in M4/M7, not M1.
