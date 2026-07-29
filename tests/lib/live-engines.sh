#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Throwaway PostgreSQL, MariaDB, Redis and MongoDB containers for the live provider
# proofs, each answering in plaintext and over TLS from a test CA.
#
#   tests/lib/live-engines.sh up          # issue the certificates, start the engines, wait
#   tests/lib/live-engines.sh env         # `export SYNQT_TEST_...=...` for each engine up
#   tests/lib/live-engines.sh github-env  # the same as KEY=VALUE lines, for $GITHUB_ENV
#   tests/lib/live-engines.sh down        # remove the containers
#
#   eval "$(tests/lib/live-engines.sh up && tests/lib/live-engines.sh env)"
#   tests/m9-providers/run-m9.sh
#
# An engine that does not come up is left out of `env`, and its proofs skip. The certificate
# names `localhost` only; a second CA that signed nothing is there to be refused. Files go
# to SYNQT_LIVE_ENGINES_DIR (default ~/.cache/synqt-live-engines), the CA key outside tls/.
#
# Ports: SYNQT_ENGINE_PG_PORT (5432), SYNQT_ENGINE_MYSQL_PORT (3306),
# SYNQT_ENGINE_REDIS_PORT (6379), SYNQT_ENGINE_REDIS_TLS_PORT (6380),
# SYNQT_ENGINE_MONGO_PORT (27017).

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
        # localhost and 127.0.0.1 (MariaDB's client reads `localhost` as its socket). 127.0.0.2
        # is left out, to show that verify-full checks the name.
        SYNQT_CERT_DAYS=30 synqt_gen_edge_cert localhost engine-ca || exit 1
        # Only what the engines read goes into tls/.
        mv engine-ca.crt wrong-ca.crt localhost.crt localhost.key tls/
        # MongoDB takes the certificate and its key as one file.
        cat tls/localhost.crt tls/localhost.key > tls/localhost.pem
        chmod 600 engine-ca.key
        chmod 644 tls/*
        rm -f wrong-ca.key
    ) || return 1
    synqt_mark_certs "$DIR/profile"
}

# The probe runs inside the container.
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

    # Copied and chowned: postgres refuses a key others can read, and the mount is read-only.
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

    # Over TCP: the image's first-start server listens on the socket only.
    ready synqt-pg psql -h 127.0.0.1 -U synqt -d synqt -c 'SELECT 1'
    ready synqt-mysql healthcheck.sh --connect
    ready synqt-redis redis-cli ping
    ready synqt-mongo mongosh --quiet --eval 'db.runCommand({ping: 1})'
    return 0
}

running() {
    [ "$(docker inspect -f '{{.State.Running}}' "$1" 2>/dev/null)" = "true" ]
}

# Asked again, so a later shell reports what is actually up.
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
