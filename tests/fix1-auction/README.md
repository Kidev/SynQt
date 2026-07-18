<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# FIX-1: the auction tutorial as an acceptance fixture

Proves the [auction tutorial](../../docs/tutorial.md)'s hands-on checks end to end on the
real `examples/gavel` system (native host kit; the edge, mesh, and database run in one
process, driven by native `SynClient`s acting as browsers). The connect-point Sources under
test are the example's own files.

Run: `./run-fix1.sh` (builds `SynQtService`/`SynQtClient` + the test with a throwaway CA and
localhost edge cert generated at configure time, then `ctest`).

`tst_fix1.cpp` verifies:

- **Hands-on check 1**: a bid that does not beat the standing one is refused *by the edge*,
  and the standing bid is untouched.
- **Hands-on check 2**: `placeBid` while signed out (as from the browser console) is refused
  by the edge, whatever the UI shows.
- The Hall-of-Fame segmentation; the auctioneer's `closeLot` records a winner in the
  database, and the database records **only** for the edge (`Caller.entity === "web"`),
  refusing a listed-but-non-edge consumer.

The third hands-on check (client-as-consumer of the database `ledger` fails `synqt check`) is
in `tools/synqt/tests/test_examples.py`.
