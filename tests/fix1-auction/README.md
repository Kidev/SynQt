<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The auction tutorial as an acceptance fixture

Proves the [auction tutorial](../../docs/tutorial.md)'s hands-on checks end to end on the
real `examples/gavel` system (native host kit, with the edge, mesh, and database in one
process, driven by native `SynClient`s acting as browsers). The connect-point Sources under
test are the example's own files.

Run: `./run-fix1.sh` (builds `SynQtEdge`/`SynQtClient` + the test with a throwaway CA and
localhost edge cert generated at configure time, then `ctest`).

`tst_fix1.cpp` verifies:

- Hands-on check 1. The edge refuses a bid that does not beat the standing one, and the
  standing bid is untouched.
- Hands-on check 2. The edge refuses `placeBid` while signed out (as from the browser
  console), whatever the UI shows.
- The Hall-of-Fame segmentation. The auctioneer's `closeLot` records a winner in the
  books entity, and it records only for the edge (`Caller.entity === "edge"`),
  refusing a listed-but-non-edge consumer.

The third hands-on check (client-as-consumer of the database `ledger` fails `synqt check`) is
in `tools/synqt/tests/test_examples.py`.
