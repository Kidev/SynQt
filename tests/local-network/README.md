<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The local test network

Two names, a loopback address each, and a development web CA. Several of the things
SynQt has to prove are browser policy questions rather than Qt questions, and a browser
will only answer them if it believes it is talking to two different sites. This is that
plumbing, in one place.

```sh
tests/local-network/local-network.sh status   # what is in place right now
tests/local-network/local-network.sh certs    # issue the CA and the server certificate
tests/local-network/local-network.sh up       # aliases, hosts entries, certificates
tests/local-network/local-network.sh down     # put the machine back
```

`certs`, `status` and `env` need no privileges. `hosts`, `aliases`, `trust` and their
inverses edit the machine, and elevate only for the file that needs it.

## Why a fake network is a real measurement here

Same-site, `SameSite`, cookie partitioning, CORS and CSP are computed from the scheme and
the registrable domain. None of them looks at where the packets went. A browser cannot
tell `https://synqtedge.test` on loopback from the same name in another country, so for
those questions this is not an approximation of the real thing, it is the real thing. It
stops being one the moment the question is latency, a proxy, or real TLS termination:
those are properties of the path, and the path here is a lie.

`.test` is reserved by RFC 6761 and can never be delegated, so no entry in
[`sites.conf`](sites.conf) can ever shadow a site that exists.

The premise is checked rather than assumed. The split-origin rig carries a `lax_control`
variant whose only job is to fail: a `SameSite=Lax` cookie must never survive a
cross-site read, and if it ever does, the two names are being treated as one site and
every other number the rig produces is void. Point it at a new engine and it validates
its own foundation before it reports anything.

## The three layers

Names, through `/etc/hosts`. It is the only mechanism that reaches every engine.
Chromium takes `--host-resolver-rules` and Firefox takes `network.dns.localDomains`, but
WebKit takes neither, and WebKit is Safari's engine, the one browser whose third-party
cookie policy this project cannot afford to guess at.

Addresses, through loopback aliases. Both sites answer on `127.0.0.1` today because
each rig binds one socket. Giving each site its own address buys real port 443 per origin
(no `:8443` in a URL) and an edge that sees its clients as separate addresses, which is
what makes its per-IP connection caps behave the way they will in production. Linux
treats all of `127.0.0.0/8` as local already; macOS needs each address added to `lo0` by
hand, which is what the `aliases` subcommand is for.

Trust, through a development web CA. The alternative is
`--ignore-certificate-errors`, which is a blunt instrument that also hides real
certificate bugs, and which Safari does not have at all. The CA is issued into
`~/.cache/synqt-local-network` (override with `SYNQT_LOCAL_NETWORK_DIR`), never into the
checkout, because a private key inside a repository is one `git add -A` from being
published.

Containers were considered and rejected. They give genuinely separate hosts, but only on
Linux runners: macOS CI has no Docker, and macOS is the leg that exists to reach Safari.
Hosts entries and aliases behave identically on both, and give up nothing on a
name-based question.

## This is not the mesh CA

The CA here signs server certificates for two fake websites so a browser will load them
without a warning. It has nothing to do with the mesh certificate authority that
authenticates entities to each other (`synqt mesh init`,
[security](../../docs/security.md)), and the two must never be crossed: this key is
issued by a test script, sits in a cache directory,
and is trusted machine-wide while you are working. Nothing that authorizes an entity may
ever chain to it.

## What has actually been run

Stated plainly, because a harness that looks tested and is not is worse than one that
admits it.

| Subcommand | Linux | macOS |
|---|---|---|
| `certs` | run; chain verified with `openssl verify`, and the CA confirmed to carry exactly one `basicConstraints` | not run |
| `hosts`, `unhosts` | logic round-tripped against a scratch file (`SYNQT_HOSTS_FILE`); run against the real `/etc/hosts` on every `browser-matrix` CI run | not run |
| `status`, `env` | run | not run |
| `aliases`, `unaliases` | no-op by design, and reported as such | **written, never run** |
| `trust`, `untrust` | **written, never run** (needs root) | **written, never run** |

The macOS column is what a VM or the `macos-15` runner is for. Until then, read every
macOS line as an unverified claim.

## Who uses it

- [`tests/split-origin`](../split-origin/README.md) takes its server certificate from
  here and needs the names mapped for its WebKit column; without them WebKit reports
  itself skipped rather than passing quietly.
- `browser-matrix.yml` calls `local-network.sh hosts` before the measurement, which is
  the only place in this project with a WebKit runtime.

The names in [`sites.conf`](sites.conf) are also written into
`tests/split-origin/measure.mjs`, which builds them into inline page scripts. Change one
and change the other.
