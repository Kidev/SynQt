#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Generate throwaway mesh certificates for the M4 topology test into $1: a project CA
# and per-entity certs for a (owner), b (consumer), and c (a valid mesh entity that is
# NOT a consumer of the connect point). Each entity cert carries a DNS SAN of its name.
#
# The profiles come from tests/lib/mesh-certs.sh, which mirrors what `synqt mesh cert`
# issues. Test material only, under build/ (git-ignored) and never committed.

set -euo pipefail

# shellcheck source=../lib/mesh-certs.sh
. "$(cd "$(dirname "$0")/../lib" && pwd)/mesh-certs.sh"

OUT="${1:?usage: gen-certs.sh <output-dir>}"
mkdir -p "$OUT"
cd "$OUT"

if synqt_certs_current .profile ca.crt a.crt b.crt c.crt; then
    exit 0
fi

synqt_gen_ca ca
synqt_gen_entity a ca
synqt_gen_entity b ca
synqt_gen_entity c ca

chmod 600 ./*.key
synqt_mark_certs .profile
