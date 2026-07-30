#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0
#
# Measure what a split-origin session cookie can rely on, and fail if the answer changes.
#
# This is a gate, not a printout. `project.origin_model: split_origin` is a hand-written
# setting whose cost is a browser policy decision, so the three findings that justify the
# documentation are asserted here. If a browser changes its mind, this test says so before
# the docs go stale.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${SPLIT_ORIGIN_WORK:-${TMPDIR:-/tmp}/synqt-split-origin}"
PLAYWRIGHT="$HERE/../m0-transport/verify/node_modules/playwright"

if [ ! -d "$PLAYWRIGHT" ]; then
    echo "SKIP: playwright is not installed; run 'npm install' in tests/m0-transport/verify"
    exit 0
fi

rm -rf "$WORK"
mkdir -p "$WORK"

# The server certificate covering both sites comes from the shared local test network, which
# is also what maps the names for WebKit. One place issues certificates for these sites, so
# the openssl subtleties that took a week to find (never `req -x509 -addext`, which has
# produced anchors with an extension twice that macOS rejects outright) live in one script
# rather than in each rig that needs a certificate.
"$HERE/../local-network/local-network.sh" certs > /dev/null
eval "$("$HERE/../local-network/local-network.sh" env)"

SPLIT_ORIGIN_CERTS="$SYNQT_LOCAL_NETWORK_DIR" node "$HERE/measure.mjs" > "$WORK/report.json"
cat "$WORK/report.json"

# Also leave the report where a CI run can archive it. The gate's pass/fail says whether the
# three documented findings still hold; it deliberately asserts nothing about WebKit, whose
# numbers have no local baseline yet. So the numbers themselves have to be readable
# afterwards, or the one engine this workflow exists to reach stays unread.
mkdir -p "$(dirname "${BASH_SOURCE[0]}")/../../build"
cp "$WORK/report.json" "$(dirname "${BASH_SOURCE[0]}")/../../build/split-origin-report.json"

python3 - "$WORK/report.json" <<'PY'
import json
import sys

report = json.load(open(sys.argv[1]))
failures = []
measured = [name for name, result in report.items()
            if "error" not in result and "skipped" not in result]


def cell(engine, variant, field):
    return report[engine][variant][field]


for engine in ("chromium", "chromium-3pc-restricted", "firefox"):
    if "error" in report.get(engine, {}):
        failures.append(f"{engine}: {report[engine]['error']}")

# WebKit is Safari's engine and the one browser whose third-party cookie policy this
# project cannot assume. It is allowed to be absent, never quietly absent.
if "skipped" in report.get("webkit", {}):
    print(f"\nNOTE: webkit not measured ({report['webkit']['skipped']})")
elif "error" in report.get("webkit", {}):
    failures.append(f"webkit: {report['webkit']['error']}")

if not failures:
    # 1. The rig discriminates. A SameSite=Lax cookie must never ride a cross-site request;
    #    if it does, the two hosts are not actually cross-site and nothing else here means
    #    anything. This caught a first version that used two names under one domain.
    for engine in measured:
        for field in ("bootstrapRead", "upgrade", "afterLoginRead"):
            if cell(engine, "lax_control", field):
                failures.append(
                    f"{engine}: a SameSite=Lax cookie crossed sites ({field}); the rig is "
                    "no longer measuring a cross-site request")

    # 2. The fragility that keeps split_origin out of the scaffold: restricting third-party
    #    cookies takes away the whole session, including the wss upgrade.
    for field in ("bootstrapRead", "upgrade", "afterLoginRead"):
        if cell("chromium-3pc-restricted", "unpartitioned", field):
            failures.append(
                f"chromium-3pc-restricted: the unpartitioned cookie survived restriction "
                f"({field}); split_origin may have stopped being fragile, so re-read the docs")

    # 3. The reason the Partitioned attribute is not shipped: it rescues the bootstrap and
    #    the upgrade but loses the login, because the OAuth callback sets the cookie under
    #    the edge's own partition. If this ever passes, CHIPS becomes addable.
    if cell("chromium", "partitioned", "afterLoginRead"):
        failures.append(
            "chromium: a Partitioned cookie set at the callback was readable from the client "
            "site; CHIPS has become viable and the edge should adopt it")
    if not cell("chromium-3pc-restricted", "partitioned", "upgrade"):
        failures.append(
            "chromium-3pc-restricted: a Partitioned cookie no longer reaches the upgrade; "
            "the documented CHIPS migration path no longer works")

if failures:
    print("\nSPLIT-ORIGIN GATE: FAIL")
    for failure in failures:
        print(f"  - {failure}")
    raise SystemExit(1)

print("\nSPLIT-ORIGIN GATE: PASS")
print("  - the Lax control fails every cross-site read, so the rig measures a real cross-site "
      "request")
print("  - an unpartitioned split-origin session dies completely under third-party cookie "
      "restriction")
print("  - Partitioned rescues the upgrade but loses the login, so it stays unshipped")
PY
