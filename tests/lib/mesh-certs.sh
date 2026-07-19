# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Shared certificate profiles for the acceptance suites. Source this, then call
# synqt_gen_ca / synqt_gen_entity / synqt_gen_edge_cert from the suite's gen-cert script.
#
# These are THROWAWAY TEST certificates. They are written under build/ (git-ignored) and are
# never committed; no production mesh CA key is created here (see docs/security.md).
#
# There is one copy of these profiles because there was previously one per suite, and they
# drifted into agreeing on something wrong: every leaf was issued with no extendedKeyUsage,
# no keyUsage and no basicConstraints, and the edge's self-signed certificate was marked
# CA:TRUE while being served as a leaf. Issuing a proper profile is right on its own terms --
# Apple's verifier does require a TLS certificate to carry the usage OID it is being used for,
# and OpenSSL only enforces an EKU that is present, which is why the old profile could be
# wrong and still keep the Linux column green.
#
# It did NOT fix macOS, and the instrument added in tst_m3.cpp to find out why has since
# answered it. macOS runs Qt on the Secure Transport backend, and it was rejecting our own
# trust anchor: "The root CA certificate is not trusted for this purpose". The dump showed the
# CA carrying *two* basicConstraints extensions, which RFC 5280 4.2 forbids ("A certificate
# MUST NOT include more than one instance of a particular extension") and Apple's verifier
# enforces, while OpenSSL shrugs and keeps the Linux column green.
#
# The duplicate came from issuing the CA with `req -x509 -addext`: -addext *adds* to whatever
# the host's openssl.cnf already puts in its x509_extensions section (typically a v3_ca that
# sets basicConstraints), it does not replace it. OpenSSL 3 happens to collapse the two;
# LibreSSL (which is what `openssl` is on macOS) emits both. So the certificate was
# malformed only on the host that then refused it, which is why this took a backend dump to
# see. The CA is now issued the same way the leaves always were, through `x509 -req -extfile`,
# where the extension file is the only source of extensions and the host's configuration
# cannot contribute a second one.
#
# Keep these in step with what `synqt mesh cert` issues (tools/synqt/synqt/mesh.py): the
# suites are only evidence about the product if they exercise the product's own profile.

# Bump this whenever a profile above changes. Every suite regenerates when the marker it
# wrote does not match, because the "are the certs still good?" guards only ever asked
# whether they had expired: a working tree built before a profile change would otherwise
# keep its old certificates until they aged out, and go on testing the profile that was
# just fixed.
SYNQT_CERT_PROFILE="3-ca-extfile"

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
        # Quiet on both streams: this one is a question, not a step. "Expired" and "not there"
        # are the normal answers that drive regeneration, and are not worth reporting.
        openssl x509 -checkend 3600 -noout -in "$crt" >/dev/null 2>&1 || return 1
    done
    return 0
}

synqt_mark_certs() { # marker-file
    printf '%s' "$SYNQT_CERT_PROFILE" > "$1"
}

# Run one openssl step: silent when it works, and when it does not, say so and show what it
# said. Every call here used to end in `>/dev/null 2>&1`, which reads like tidiness and is not:
# it discards the diagnostic and the exit status goes unremarked, so a step that half-worked
# produced a subtly wrong certificate and the first sign of it was a TLS handshake failing three
# suites later. Capturing rather than passing stderr through keeps a green run quiet; openssl
# reports "Certificate request self-signature ok" on stderr as a matter of course.
#
# MSYS2_ARG_CONV_EXCL is for Git Bash on Windows. Its runtime rewrites arguments that look
# like absolute POSIX paths into Windows paths before handing them to a native program, and a
# subject like /CN=ca looks exactly like one: openssl received 'C:/Program Files/Git/CN=ca'
# and refused it ("subject name is expected to be in the format /type0=value0/..."), which
# failed seven suites at configure time on the Windows column alone. The exclusion is scoped
# to the /CN= prefix rather than disabling conversion wholesale, because the surrounding
# arguments are real paths that do need translating. It is an unset variable everywhere else.
_synqt_openssl() {
    _out="$(MSYS2_ARG_CONV_EXCL='/CN=' openssl "$@" 2>&1)" || {
        echo "openssl $1 failed:" >&2
        printf '%s\n' "$_out" >&2
        return 1
    }
    return 0
}

# Assert a freshly issued certificate really carries an extension we asked for.
#
# Asking is not getting. These scripts run on whatever `openssl` the host has on PATH, and
# that is not one program: macOS ships LibreSSL under the name, Windows runners have their
# own build, and the extension flags below are exactly the area where those implementations
# have differed. A certificate that silently comes out without its extensions does not fail
# here; it fails later, as a TLS handshake that times out on one platform with no reason
# given, which is a much more expensive way to learn the same thing.
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

# Assert an extension appears exactly once.
#
# Presence is not the whole profile. RFC 5280 4.2 forbids a certificate from carrying the same
# extension twice, and Apple's verifier holds it to that: a CA with two basicConstraints is
# refused as a trust anchor, so every macOS suite that pinned it failed while OpenSSL-based
# hosts accepted the same file. synqt_assert_cert_ext could not see it: a duplicate matches
# "is it there?" twice over, so the malformed certificate passed its own profile check and
# only spoke up as a TLS handshake failing on one platform. Counting is the difference.
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

# A private CA: the trust anchor a mesh (or a pinned edge) verifies against.
#
# Self-signed through a CSR and an extension file rather than in one `req -x509 -addext` step.
# The one-step form inherits the host openssl.cnf's x509_extensions section and adds to it, so
# the resulting CA carried whatever that section said *plus* what we asked for; on LibreSSL,
# two basicConstraints, which macOS then refused as an anchor (see the header). `x509 -req`
# takes its extensions only from -extfile, so this is the same profile on every host's openssl.
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

# A mesh entity leaf, signed by a CA. serverAuth and clientAuth both: an entity is the TLS
# server on the links it owns and the TLS client on the links it consumes, presenting this
# one certificate in both directions. The subject is the entity name, which is what the
# owner reads back as the calling entity.
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

# The edge's public-facing certificate, signed by a CA the client pins. serverAuth only:
# the browser (or the native client) is never asked for a certificate on this link, it
# authenticates with a user session.
#
# This is a CA-issued leaf rather than a self-signed certificate pinned as its own anchor,
# because that is the shape a real edge deploys and it leaves no question about whether a
# leaf may also be its own trust root.
synqt_gen_edge_cert() { # name signing-ca
    _synqt_openssl genrsa -out "$1.key" 2048 || return 1
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
