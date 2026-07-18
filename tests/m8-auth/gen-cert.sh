#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Throwaway mesh certificates for the M8 provider_entity test into $1:
#   ca                 - the project mesh CA
#   auth, web, web2    - entity certs (SAN DNS:<name>), signed by ca
#
# The profiles come from tests/lib/mesh-certs.sh, which mirrors what `synqt mesh cert`
# issues. TEST certificates only, under build/ (git-ignored), never committed.

set -euo pipefail

# shellcheck source=../lib/mesh-certs.sh
. "$(cd "$(dirname "$0")/../lib" && pwd)/mesh-certs.sh"

OUT="${1:?usage: gen-cert.sh <output-dir>}"
mkdir -p "$OUT"
cd "$OUT"

if synqt_certs_current .profile ca.crt auth.crt web.crt web2.crt; then
    exit 0
fi

synqt_gen_ca ca
synqt_gen_entity auth ca
synqt_gen_entity web ca
synqt_gen_entity web2 ca

chmod 600 ./*.key
synqt_mark_certs .profile
