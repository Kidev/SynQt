<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Split-origin: what the session cookie can rely on

`project.origin_model: split_origin` puts the client on one site and the edge on
another, so the session cookie is a third-party cookie. Whether that works is a browser
policy decision and not a Qt one, and it is why split-origin is a setting you write by
hand instead of one `synqt new` offers.

This suite measures the policy, so the documentation quotes data instead of folklore. Run
it with `./run-split-origin.sh`.

## Verdict

Split-origin works in current browsers and stops working the moment third-party
cookies are restricted. It does not degrade. The client loads from the CDN, the browser
ignores the session request, the `wss` upgrade arrives with no credential, and the edge
refuses it. The app is on screen and permanently disconnected.

The `Partitioned` (CHIPS) attribute is not the fix as things stand. It rescues the
bootstrap and the upgrade under restriction, and it breaks login everywhere, because the
OAuth callback is a top-level navigation that lands on the edge. The browser stores the
cookie under the edge's own partition, and the client site can never read it back. Adding
the attribute would trade a path that works today for one that fails today, so the edge
does not emit it.

Measured 2026-07-28 on one workstation (Chromium 149.0.7827.55 and Firefox
151.0), and 2026-07-31 in CI, which adds WebKit 26.5 on Linux and macOS runners. All of
it on two separate registrable domains over TLS, with the Playwright builds.

| browser | cookie | bootstrap read | wss upgrade | login |
|---|---|---|---|---|
| Chromium | `SameSite=None` (what the edge emits) | pass | pass | pass |
| Chromium | `+ Partitioned` | pass | pass | fail |
| Chromium, third-party cookies restricted | `SameSite=None` | fail | fail | fail |
| Chromium, third-party cookies restricted | `+ Partitioned` | pass | pass | fail |
| Firefox | `SameSite=None` | pass | pass | pass |
| Firefox | `+ Partitioned` | pass | pass | pass |
| WebKit | `SameSite=None` | fail | fail | see below |
| WebKit | `+ Partitioned` | fail | fail | see below |
| all three | `SameSite=Lax` (control) | fail | fail | fail |

Two readings of that table matter:

- Chromium reports the partition key it stored, and for the login row it is
  `https://synqtedge.test`, the edge's site. That names the mechanism, and it is not an
  inference from the failure.
- Firefox stores the `Partitioned` cookie with no partition key, so it did not apply
  CHIPS at all. Its pass in that row is the unpartitioned behavior wearing a different
  attribute, which is a second reason not to rely on the attribute yet.

WebKit has been measured in CI since 2026-07-31. On a machine with no WebKit runtime
the rig reports it as skipped instead of passing over it in silence, and WebKit has
neither a host resolver flag nor a DNS pref, so it needs the names mapped.
[`browser-matrix.yml`](../../.github/workflows/browser-matrix.yml) already
installs WebKit for the M0 matrix, so it maps the two sites into `/etc/hosts` and runs
this gate on a Linux and a macOS runner, and both agree:

- The cross-site half is dead. Neither the bootstrap read nor the `wss` upgrade sees
  the session, with or without `Partitioned`. The browser stores a cookie, with a null
  partition key, so this is not CHIPS at work, and the client site cannot read it back.
  That is the restricted-Chromium row, in WebKit's default configuration, today.
- The login read is the one place the two runners disagreed. The cookie set during the
  top-level navigation to the edge was readable from the client page afterwards on the
  Linux runner and not on the macOS one. Nothing rests on which is right, because a
  session that cannot be read by a cross-site fetch cannot reach the `wss` upgrade
  either way, and that is the connection the whole mode exists to make.

So split-origin is already broken in Safari's engine, and it is not only at risk from a
policy that is coming. That is measured.

## What would make split-origin durable

The callback has to stop writing the session cookie. It would redirect to the client
site carrying a one-time code instead, and the boot script would exchange that code with a
credentialed request from the client context, which writes the cookie in the client site's
partition. Every column above then passes under restriction, with `Partitioned` on.

That is not built. Split-origin is a hand-written setting for people who have read this
page. The direction that removes the problem instead of managing it is to put the client
and the edge back on one site (see the load distribution note in
[project layout and config](../../docs/project-layout-and-config.md)).

## How it works, and why the control matters

Two sites resolve to loopback and share one certificate. `synqtcdn.test` delivers the
client and `synqtedge.test` is the edge, mapped through `--host-resolver-rules` in
Chromium and `network.dns.localDomains` in Firefox. The names and the certificate come
from [the shared local test network](../local-network/README.md), which also maps them for
WebKit, since WebKit has no resolver override of its own. Run
`tests/local-network/local-network.sh hosts` once and the WebKit column stops skipping
here too.

They have to be separate registrable domains. A browser applies no third-party rule to
two names under one site, such as `cdn.synqt.test` and `app.synqt.test`, and every single
cell then comes back as working. The `lax_control` variant makes that failure loud. A
`SameSite=Lax` cookie must never cross sites, so if the control ever passes, the rig has
stopped measuring and the gate fails instead of reporting good news.

The gate asserts three things, each mutation-tested to confirm it fails when violated.
The control never crosses sites. The unpartitioned cookie dies under restriction. And
`Partitioned` loses the login while keeping the upgrade. If a browser changes any of them,
this test says so before the documentation goes stale.
