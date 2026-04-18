# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Shared certificate profiles for the acceptance suites. Source this, then call
# synqt_gen_ca / synqt_gen_entity / synqt_gen_edge_cert from the suite's gen-cert script.
#
# Throwaway test certificates, written under build/ and never committed. No production mesh
# CA key is created here (docs/security.md).
#
# Every certificate, the CA included, is issued through `x509 -req -extfile`, so the
# extension file is the only source of extensions on every host's openssl.
#
# Keep these in step with what `synqt mesh cert` issues (tools/synqt/synqt/mesh.py).

# Bump this whenever a profile changes: every suite regenerates on a mismatch.
SYNQT_CERT_PROFILE="4-ec-edge-leaf"

# Usage: synqt_certs_current <marker-file> <cert> [cert...]
# True when the marker matches this profile and every named cert exists and is not about
# to expire.
synqt_certs_current() {
    local marker="$1"
    shift
    [ -f "$marker" ] || return 1
    [ "$(cat "$marker")" = "$SYNQT_CERT_PROFILE" ] || return 1
    local crt
    for crt in "$@"; do
        [ -f "$crt" ] || return 1
        openssl x509 -checkend 3600 -noout -in "$crt" >/dev/null 2>&1 || return 1
    done
    return 0
}

synqt_mark_certs() { # marker-file
    printf '%s' "$SYNQT_CERT_PROFILE" > "$1"
}

# Run one openssl step: silent on success, its output shown on failure.
# MSYS2_ARG_CONV_EXCL stops Git Bash rewriting a /CN= subject into a Windows path.
_synqt_openssl() {
    _out="$(MSYS2_ARG_CONV_EXCL='/CN=' openssl "$@" 2>&1)" || {
        echo "openssl $1 failed:" >&2
        printf '%s\n' "$_out" >&2
        return 1
    }
    return 0
}

# Assert a freshly issued certificate carries an extension that was asked for: the host's
# openssl may be LibreSSL or another build.
#
#   synqt_assert_cert_ext <cert> <text-that-must-appear-in-the-x509-dump>
synqt_assert_cert_ext() {
    if ! openssl x509 -in "$1" -noout -text 2>/dev/null | grep -qi -- "$2"; then
        echo "cert profile error: $1 is missing '$2'" >&2
        echo "  issued by: $(openssl version 2>/dev/null || echo 'unknown openssl')" >&2
        return 1
    fi
    return 0
}

# Assert an extension appears exactly once (RFC 5280 4.2; Apple's verifier enforces it).
#
#   synqt_assert_cert_ext_once <cert> <extension-name-as-openssl-prints-it>
synqt_assert_cert_ext_once() {
    local seen
    seen="$(openssl x509 -in "$1" -noout -text 2>/dev/null | grep -ci -- "$2")"
    if [ "$seen" != "1" ]; then
        echo "cert profile error: $1 has $seen copies of '$2', expected exactly 1" >&2
        echo "  issued by: $(openssl version 2>/dev/null || echo 'unknown openssl')" >&2
        echo "  (a repeated extension is invalid per RFC 5280 4.2 and macOS rejects it)" >&2
        return 1
    fi
    return 0
}

# A private CA: the trust anchor a mesh (or a pinned edge) verifies against. Through a CSR
# and -extfile, never `req -x509 -addext`, which adds to the host openssl.cnf's extensions.
synqt_gen_ca() { # name
    _synqt_openssl genrsa -out "$1.key" 2048 || return 1
    _synqt_openssl req -new -key "$1.key" -subj "/CN=$1" -out "$1.csr" || return 1
    printf '%s\n' \
        "basicConstraints=critical,CA:TRUE" \
        "keyUsage=critical,keyCertSign,cRLSign" \
        "subjectKeyIdentifier=hash" \
        "authorityKeyIdentifier=keyid:always" > "$1.ext"
    _synqt_openssl x509 -req -in "$1.csr" -signkey "$1.key" -sha256 \
        -days "${SYNQT_CERT_DAYS:-5}" -extfile "$1.ext" -out "$1.crt" || return 1
    rm -f "$1.csr" "$1.ext"
    synqt_assert_cert_ext "$1.crt" "CA:TRUE" || return 1
    synqt_assert_cert_ext "$1.crt" "Certificate Sign" || return 1
    synqt_assert_cert_ext_once "$1.crt" "X509v3 Basic Constraints" || return 1
}

# A mesh entity leaf, signed by a CA. serverAuth and clientAuth: an entity presents one
# certificate on the links it owns and those it consumes. The subject is the entity name.
synqt_gen_entity() { # name signing-ca
    _synqt_openssl genrsa -out "$1.key" 2048 || return 1
    _synqt_openssl req -new -key "$1.key" -subj "/CN=$1" -out "$1.csr" || return 1
    printf '%s\n' \
        "basicConstraints=critical,CA:FALSE" \
        "keyUsage=critical,digitalSignature,keyEncipherment" \
        "extendedKeyUsage=serverAuth,clientAuth" \
        "subjectAltName=DNS:$1" \
        "subjectKeyIdentifier=hash" \
        "authorityKeyIdentifier=keyid,issuer" > "$1.ext"
    _synqt_openssl x509 -req -in "$1.csr" -CA "$2.crt" -CAkey "$2.key" -CAcreateserial \
        -days "${SYNQT_CERT_DAYS:-5}" -sha256 \
        -extfile "$1.ext" -out "$1.crt" || return 1
    rm -f "$1.csr" "$1.ext"
    synqt_assert_cert_ext "$1.crt" "TLS Web Server Authentication" || return 1
    synqt_assert_cert_ext "$1.crt" "TLS Web Client Authentication" || return 1
}

# The edge's public-facing certificate: a CA-issued leaf the client pins. serverAuth only;
# the browser authenticates with a session.
synqt_gen_edge_cert() { # name signing-ca
    _synqt_openssl genrsa -out "$1.key" 2048 || return 1
    _synqt_gen_edge_leaf "$1" "$2"
}

# The same edge certificate over an elliptic-curve key, as ACME `--key-type ecdsa` writes.
synqt_gen_edge_cert_ec() { # name signing-ca
    _synqt_openssl ecparam -name prime256v1 -genkey -noout -out "$1.key" || return 1
    _synqt_gen_edge_leaf "$1" "$2"
}

# The leaf both of the two above issue, given a key that is already written.
_synqt_gen_edge_leaf() { # name signing-ca
    _synqt_openssl req -new -key "$1.key" -subj "/CN=localhost" -out "$1.csr" || return 1
    printf '%s\n' \
        "basicConstraints=critical,CA:FALSE" \
        "keyUsage=critical,digitalSignature,keyEncipherment" \
        "extendedKeyUsage=serverAuth" \
        "subjectAltName=DNS:localhost,IP:127.0.0.1" \
        "subjectKeyIdentifier=hash" \
        "authorityKeyIdentifier=keyid,issuer" > "$1.ext"
    _synqt_openssl x509 -req -in "$1.csr" -CA "$2.crt" -CAkey "$2.key" -CAcreateserial \
        -days "${SYNQT_CERT_DAYS:-5}" -sha256 \
        -extfile "$1.ext" -out "$1.crt" || return 1
    rm -f "$1.csr" "$1.ext"
    synqt_assert_cert_ext "$1.crt" "TLS Web Server Authentication" || return 1
}
