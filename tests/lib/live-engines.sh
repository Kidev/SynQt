#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The throwaway engines the live provider proofs run against: PostgreSQL, MariaDB, Redis and
# MongoDB, in containers, each answering in plaintext and over TLS from a test CA.
#
#   tests/lib/live-engines.sh up          # issue the certificates, start the engines, wait
#   tests/lib/live-engines.sh env         # `export SYNQT_TEST_...=...` for each engine up
#   tests/lib/live-engines.sh github-env  # the same as KEY=VALUE lines, for $GITHUB_ENV
#   tests/lib/live-engines.sh down        # remove the containers
#
#   eval "$(tests/lib/live-engines.sh up && tests/lib/live-engines.sh env)"
#   tests/m9-providers/run-m9.sh
#
# CI and a workstation run this same script, so a live proof that passes in one passes
# against the same engines, versions and certificates in the other. An engine that does not
# come up is reported and left out of `env`, and the proofs that need it skip.
#
# TLS is on beside plaintext, not instead of it. The plaintext port is what the swap proofs
# use, and it is also the server a `tls: true` provider must refuse to settle for. The
# certificate names `localhost` and nothing else, so a proof can connect by name and
# verify, and connect by address to show that verify-full checks the name. A second CA that
# signed nothing is issued beside the real one, so a proof can hand a provider the wrong
# anchor and watch it refuse.
#
# The certificates are test material, written to SYNQT_LIVE_ENGINES_DIR (default
# ~/.cache/synqt-live-engines) and never into the checkout. They have nothing to do with a
# mesh CA. The engines read their files as their own unprivileged users, so what they need
# sits in a readable tls/ inside a directory private to the user running this, and the CA's
# key stays outside tls/.
#
# Ports can be moved when a machine already uses one: SYNQT_ENGINE_PG_PORT (5432),
# SYNQT_ENGINE_MYSQL_PORT (3306), SYNQT_ENGINE_REDIS_PORT (6379), SYNQT_ENGINE_REDIS_TLS_PORT
# (6380) and SYNQT_ENGINE_MONGO_PORT (27017).

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
DIR="${SYNQT_LIVE_ENGINES_DIR:-$HOME/.cache/synqt-live-engines}"
TLS="$DIR/tls"
PG_PORT="${SYNQT_ENGINE_PG_PORT:-5432}"
MYSQL_PORT="${SYNQT_ENGINE_MYSQL_PORT:-3306}"
REDIS_PORT="${SYNQT_ENGINE_REDIS_PORT:-6379}"
REDIS_TLS_PORT="${SYNQT_ENGINE_REDIS_TLS_PORT:-6380}"
MONGO_PORT="${SYNQT_ENGINE_MONGO_PORT:-27017}"
ENGINES="synqt-pg synqt-mysql synqt-redis synqt-mongo"

# shellcheck source=tests/lib/mesh-certs.sh
. "$HERE/mesh-certs.sh"

say() { printf 'live-engines: %s\n' "$*" >&2; }

issue_certificates() {
    mkdir -p "$DIR/tls"
    chmod 700 "$DIR"
    chmod 755 "$DIR/tls"
    if synqt_certs_current "$DIR/profile" "$TLS/engine-ca.crt" "$TLS/localhost.crt" \
            "$TLS/wrong-ca.crt"; then
        return 0
    fi
    (
        cd "$DIR" || exit 1
        rm -f ./*.crt ./*.key ./*.srl tls/*
        SYNQT_CERT_DAYS=30 synqt_gen_ca engine-ca || exit 1
        SYNQT_CERT_DAYS=30 synqt_gen_ca wrong-ca || exit 1
        issue_server_certificate || exit 1
        # What the engines read goes into tls/, and only that. The CA's key stays in the
        # private directory above it, and the wrong CA's key is not needed at all.
        mv engine-ca.crt wrong-ca.crt localhost.crt localhost.key tls/
        # MongoDB takes the certificate and its key as one file.
        cat tls/localhost.crt tls/localhost.key > tls/localhost.pem
        chmod 600 engine-ca.key
        chmod 644 tls/*
        rm -f wrong-ca.key
    ) || return 1
    synqt_mark_certs "$DIR/profile"
}

# The engines' certificate, in the shared leaf profile, naming localhost and 127.0.0.1. The
# address is there because MariaDB's client reads `localhost` as its Unix socket, so a
# verified MySQL connection has to be made by address; 127.0.0.2 is left out on purpose, as
# the loopback address a proof connects through to show that verify-full checks the name.
issue_server_certificate() {
    _synqt_openssl genrsa -out localhost.key 2048 || return 1
    _synqt_openssl req -new -key localhost.key -subj "/CN=localhost" -out localhost.csr \
        || return 1
    printf '%s\n' \
        "basicConstraints=critical,CA:FALSE" \
        "keyUsage=critical,digitalSignature,keyEncipherment" \
        "extendedKeyUsage=serverAuth" \
        "subjectAltName=DNS:localhost,IP:127.0.0.1" \
        "subjectKeyIdentifier=hash" \
        "authorityKeyIdentifier=keyid,issuer" > localhost.ext
    _synqt_openssl x509 -req -in localhost.csr -CA engine-ca.crt -CAkey engine-ca.key \
        -CAcreateserial -days 30 -sha256 -extfile localhost.ext -out localhost.crt || return 1
    rm -f localhost.csr localhost.ext
    synqt_assert_cert_ext localhost.crt "TLS Web Server Authentication"
}

# Name and probe. The probe runs inside the container, so it asks the engine itself and not
# whatever else might be listening on the published port.
ready() {
    local name="$1"
    shift
    local _
    for _ in $(seq 1 60); do
        if docker exec "$name" "$@" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    say "$name did not become ready; its live proofs will skip. Its log ends:"
    docker logs --tail 20 "$name" 2>&1 | sed 's/^/  | /' >&2
    return 1
}

up() {
    if ! command -v docker >/dev/null 2>&1; then
        say "docker is not installed; every live proof will skip"
        return 1
    fi
    issue_certificates || { say "the test certificates could not be issued"; return 1; }
    docker rm -f $ENGINES >/dev/null 2>&1

    # The key is copied in and handed to the postgres user, because the server refuses a key
    # anyone else can read and the mount is read-only.
    docker run -d --name synqt-pg -e POSTGRES_PASSWORD=synqt -e POSTGRES_USER=synqt \
        -e POSTGRES_DB=synqt -p "$PG_PORT:5432" -v "$TLS:/certs:ro" postgres:16 \
        bash -c 'install -o postgres -m 600 /certs/localhost.key /var/lib/postgresql/tls.key &&
                 install -o postgres -m 644 /certs/localhost.crt /var/lib/postgresql/tls.crt &&
                 exec docker-entrypoint.sh "$@"' -- \
        postgres -c ssl=on -c ssl_cert_file=/var/lib/postgresql/tls.crt \
        -c ssl_key_file=/var/lib/postgresql/tls.key >/dev/null
    # MariaDB, never Oracle's image. The provider is built against Connector/C.
    docker run -d --name synqt-mysql -e MARIADB_ROOT_PASSWORD=synqt \
        -e MARIADB_USER=synqt -e MARIADB_PASSWORD=synqt -e MARIADB_DATABASE=synqt \
        -p "$MYSQL_PORT:3306" -v "$TLS:/certs:ro" mariadb:11 \
        --ssl-cert=/certs/localhost.crt --ssl-key=/certs/localhost.key \
        --ssl-ca=/certs/engine-ca.crt >/dev/null
    docker run -d --name synqt-redis -p "$REDIS_PORT:6379" -p "$REDIS_TLS_PORT:6380" \
        -v "$TLS:/certs:ro" redis:7 redis-server --port 6379 --tls-port 6380 \
        --tls-cert-file /certs/localhost.crt --tls-key-file /certs/localhost.key \
        --tls-ca-cert-file /certs/engine-ca.crt --tls-auth-clients no >/dev/null
    docker run -d --name synqt-mongo -p "$MONGO_PORT:27017" -v "$TLS:/certs:ro" mongo:7 \
        --tlsMode allowTLS --tlsCertificateKeyFile /certs/localhost.pem \
        --tlsCAFile /certs/engine-ca.crt --tlsAllowConnectionsWithoutCertificates >/dev/null

    # Over TCP, not the socket: during its first start the postgres image runs a private
    # server on the socket alone, which answers pg_isready and then goes away.
    ready synqt-pg psql -h 127.0.0.1 -U synqt -d synqt -c 'SELECT 1'
    ready synqt-mysql healthcheck.sh --connect
    ready synqt-redis redis-cli ping
    ready synqt-mongo mongosh --quiet --eval 'db.runCommand({ping: 1})'
    return 0
}

running() {
    [ "$(docker inspect -f '{{.State.Running}}' "$1" 2>/dev/null)" = "true" ]
}

# KEY=VALUE lines for the engines that are up now. Asked again rather than remembered from
# `up`, so `env` in a later shell reports what is actually there.
variables() {
    local any=0
    if running synqt-pg && docker exec synqt-pg psql -h 127.0.0.1 -U synqt -d synqt \
            -c 'SELECT 1' >/dev/null 2>&1; then
        any=1
        printf '%s\n' SYNQT_TEST_PG_HOST=127.0.0.1 "SYNQT_TEST_PG_PORT=$PG_PORT" \
            SYNQT_TEST_PG_DB=synqt SYNQT_TEST_PG_USER=synqt SYNQT_TEST_PG_PASSWORD=synqt
    fi
    if running synqt-mysql && docker exec synqt-mysql healthcheck.sh --connect >/dev/null 2>&1
    then
        any=1
        printf '%s\n' SYNQT_TEST_MYSQL_HOST=127.0.0.1 "SYNQT_TEST_MYSQL_PORT=$MYSQL_PORT" \
            SYNQT_TEST_MYSQL_DB=synqt SYNQT_TEST_MYSQL_USER=synqt SYNQT_TEST_MYSQL_PASSWORD=synqt
    fi
    if running synqt-redis && docker exec synqt-redis redis-cli ping >/dev/null 2>&1; then
        any=1
        printf '%s\n' SYNQT_TEST_REDIS_HOST=127.0.0.1 "SYNQT_TEST_REDIS_PORT=$REDIS_PORT" \
            "SYNQT_TEST_REDIS_TLS_PORT=$REDIS_TLS_PORT"
    fi
    if running synqt-mongo && docker exec synqt-mongo mongosh --quiet \
            --eval 'db.runCommand({ping: 1})' >/dev/null 2>&1; then
        any=1
        printf '%s\n' "SYNQT_TEST_MONGO_URI=mongodb://127.0.0.1:$MONGO_PORT" \
            SYNQT_TEST_MONGO_DB=synqt "SYNQT_TEST_MONGO_PORT=$MONGO_PORT"
    fi
    if [ "$any" = 1 ]; then
        printf '%s\n' "SYNQT_TEST_ENGINE_CA=$TLS/engine-ca.crt" \
            "SYNQT_TEST_ENGINE_WRONG_CA=$TLS/wrong-ca.crt"
    fi
}

case "${1:-}" in
    up) up ;;
    env) variables | sed 's/^/export /' ;;
    github-env) variables ;;
    down) docker rm -f $ENGINES >/dev/null 2>&1; exit 0 ;;
    *)
        echo "usage: live-engines.sh up|env|github-env|down" >&2
        exit 2
        ;;
esac
