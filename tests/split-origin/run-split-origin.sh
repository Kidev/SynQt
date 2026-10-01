#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0
#
# Assert the three findings about a split-origin session cookie that the docs rely on.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${SPLIT_ORIGIN_WORK:-${TMPDIR:-/tmp}/synqt-split-origin}"
PLAYWRIGHT="$HERE/../transport-spike/verify/node_modules/playwright"

if [ ! -d "$PLAYWRIGHT" ]; then
    echo "SKIP: playwright is not installed; run 'npm install' in tests/transport-spike/verify"
    exit 0
fi

rm -rf "$WORK"
mkdir -p "$WORK"

# The server certificate for both sites comes from the local test network.
"$HERE/../local-network/local-network.sh" certs > /dev/null
eval "$("$HERE/../local-network/local-network.sh" env)"

SPLIT_ORIGIN_CERTS="$SYNQT_LOCAL_NETWORK_DIR" node "$HERE/measure.mjs" > "$WORK/report.json"
cat "$WORK/report.json"

# Leave the report for CI to archive; the WebKit numbers are reported, not asserted.
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
    # 1. A SameSite=Lax cookie must never ride a cross-site request, or the two hosts are not
    #    cross-site.
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

    # 3. Partitioned rescues the bootstrap and the upgrade but loses the login. If this
    #    ever passes, CHIPS becomes addable.
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
