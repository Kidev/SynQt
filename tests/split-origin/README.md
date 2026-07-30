<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Split-origin: what the session cookie can rely on

`project.origin_model: split_origin` puts the client on one site and the edge on
another, so the session cookie is a third-party cookie. Whether that works is a
browser policy decision, not a Qt one, and it is the whole reason split-origin is a
setting you write by hand rather than one `synqt new` offers.

This measures the policy, so the documentation quotes data instead of folklore. Run
it with `./run-split-origin.sh`.

## Verdict

**Split-origin works in current browsers and stops working the moment third-party
cookies are restricted.** Not "degrades": the client loads from the CDN, the session
request is ignored, the `wss` upgrade arrives with no credential, and the edge
refuses it. The app is on screen and permanently disconnected.

**The `Partitioned` (CHIPS) attribute is not the fix, as things stand.** It rescues
the bootstrap and the upgrade under restriction, and it breaks login everywhere,
because the OAuth callback is a top-level navigation that lands on the edge. The
cookie is stored under the edge's own partition, and the client site can never read
it back. Adding the attribute would trade a path that works today for one that fails
today, so the edge does not emit it.

Measured 2026-07-28, Chromium 149.0.7827.55 and Firefox 151.0 (the Playwright
builds), on two separate registrable domains over TLS.

| browser | cookie | bootstrap read | wss upgrade | login |
|---|---|---|---|---|
| Chromium | `SameSite=None` (what the edge emits) | pass | pass | pass |
| Chromium | `+ Partitioned` | pass | pass | **fail** |
| Chromium, third-party cookies restricted | `SameSite=None` | **fail** | **fail** | **fail** |
| Chromium, third-party cookies restricted | `+ Partitioned` | pass | pass | **fail** |
| Firefox | `SameSite=None` | pass | pass | pass |
| Firefox | `+ Partitioned` | pass | pass | pass |
| all three | `SameSite=Lax` (control) | fail | fail | fail |

Two readings of that table are load bearing:

- Chromium reports the partition key it stored, and for the login row it is
  `https://synqtedge.test`, the edge's site. That is the mechanism, not an inference
  from the failure.
- Firefox stores the `Partitioned` cookie with **no** partition key, so it did not
  apply CHIPS at all. Its "pass" in that row is the unpartitioned behavior wearing a
  different attribute, which is a second reason not to rely on the attribute yet.

**Safari and WebKit are not measured in the table above.** There is no WebKit runtime
on the development machine, and WebKit has neither a host resolver flag nor a DNS
pref, so the rig reports it as skipped rather than passing it over in silence.

CI closes that: [`browser-matrix.yml`](../../.github/workflows/browser-matrix.yml)
already installs WebKit for the M0 matrix, so it maps the two sites into `/etc/hosts`
and runs this gate on Linux and macOS runners, where WebKit is measurable. Until that
run has been read, the expectation below is inference and must not be quoted as
measurement: Safari blocks third-party cookies by default, so it is *expected* to
behave like the restricted Chromium rows, which would mean split-origin is already
broken there today.

## What would make split-origin durable

The callback must stop being the thing that writes the session cookie. Instead it
would redirect to the client site carrying a one-time code, and the boot script would
exchange that code with a credentialed request from the client context, which writes
the cookie in the client site's partition. Every column above then passes under
restriction, with `Partitioned` on.

It is not built. Split-origin is a hand-written setting for people who have read this
page, and the direction that removes the problem instead of managing it is to put the
client and the edge back on one site (see the load distribution note in
[project layout and config](../../docs/project-layout-and-config.md)).

## How it works, and why the control matters

Two sites resolve to loopback: `synqtcdn.test` delivers the client, `synqtedge.test`
is the edge (Chromium via `--host-resolver-rules`, Firefox via
`network.dns.localDomains`), sharing one certificate. The names and the certificate come
from [the shared local test network](../local-network/README.md), which is also what maps
them for WebKit, since WebKit has no resolver override of its own. Run
`tests/local-network/local-network.sh hosts` once and the WebKit column stops skipping
here too.

They must be separate **registrable domains**. The first version of this rig used
`cdn.synqt.test` and `app.synqt.test`, which are two names under one site, so no
third-party rule applied and every single cell came back "works". The `lax_control`
variant exists to make that failure loud: a `SameSite=Lax` cookie must never cross
sites, so if the control ever passes, the rig has stopped measuring and the gate
fails rather than reporting good news.

The gate asserts three things, each mutation-tested to confirm it fails when
violated: the control never crosses sites; the unpartitioned cookie dies under
restriction; and `Partitioned` loses the login but keeps the upgrade. If a browser
changes any of them, this test says so before the documentation goes stale.
