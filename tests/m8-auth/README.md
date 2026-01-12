<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M8: identity and easy auth

The whole OAuth2 / OpenID Connect Authorization Code flow runs on the web edge; the
browser only ever ends with an httpOnly session cookie.

## What runs

- `IdentityProvider` (`src/edge`); `QOAuth2AuthorizationCodeFlow` with PKCE, a
  framework-generated state verified on the callback (a state the edge did not issue is
  refused before any token exchange), the token exchange with the edge-held
  `client_secret`, identity normalization per [authentication](../../docs/authentication.md), the scope-mapping
  QML hook (`IdentityMapping.scopeFor`), the authenticated session, and the
  `HttpOnly`/`Secure`/`SameSite` cookie. A custom `EdgeReplyHandler` gives the flow the
  edge's public callback URL instead of a loopback port.
- `JwksVerifier` (`src/identity`); for OpenID Connect providers, verifies the ID token's
  RS256 signature against the provider JWKS (fetched and cached with `QNetworkAccessManager`)
  plus iss/aud/exp/nonce, using pinned jwt-cpp (MIT, vcpkg). No hand-rolled crypto; the
  signature is checked with the no-throw `rs256::verify(..., ec)` so no exception crosses the
  boundary.
- `StubIdentityServer` (`src/edge`); a dev-only provider (`/authorize`, `/token`,
  `/userinfo`, `/jwks`) that authenticates a preconfigured user, verifies the PKCE S256
  verifier and the client secret, and issues a real RS256-signed ID token. It is gated so it
  can never ship: it takes a `DevOnly` acknowledgement, and the runtime refuses a `devStub`
  provider unless `identity.allow_dev_stub` is on (only `synqt dev` sets it).

## What the tests check (`tst_m8.cpp`)

1. fullLoginFlow: login -> provider -> callback creates a session; the cookie is
   `HttpOnly`; the session carries the normalized identity and the scope the map hook
   returned (`moderator` for octocat); the access token is held on the edge, is not the
   cookie value, and appears in neither the `Set-Cookie` nor the response body.
2. oidcLoginVerifiesIdToken: an OIDC provider with no userinfo endpoint: a session
   is created only because the ID token's signature verified against the JWKS; the identity
   comes from the ID-token claims and the auth request carried a nonce.
3. oidcWrongIssuerRejected: an ID token whose `iss` does not match fails verification;
   no session.
4. unknownStateRejected: a forged state is refused (400) before any token exchange.
5. devStubRefusedWithoutGate: with the dev gate off, the dev stub provider is refused
   (403).

## The desktop half (`tst_desktop.cpp`)

A native app has no origin, so the finished session cannot be handed back as a cookie. It
comes back over a loopback redirect instead (the native-app pattern of RFC 8252), and what
crosses that redirect is a one-time claim code rather than the session, because the URL a
browser was sent to is written into that browser's history. See
[desktop](../../docs/desktop.md).

`LoopbackReceiver` (`src/client`) is the listener, bound to `127.0.0.1` and closed the
moment the answer arrives. `IdentityProvider::handleClaim` is the exchange, served at
`<login route>/claim`, POST only.

What the suite proves, in the order it would hurt to get wrong:

1. `returnMustBeLoopback` and `returnNeedsItsNonceAndChallenge`. The `return` URL is an
   allowlist of one shape, checked before the provider is contacted. Thirteen refusals
   including `http://127.0.0.1@evil.example/`, `localhost` by name, and a return carrying a
   path, a query or a fragment of its own. An open redirect here hands out sessions.
2. `claimIsSingleUseAndBoundToItsVerifier`. The redirect carries a code and the app's own
   nonce, the browser is left with no session cookie, and the code is spent by the first
   attempt whether or not that attempt had the right verifier.
3. `claimBuysTheSessionOnce`. The right verifier buys a real session carrying the
   provider's identity, answered `no-store`, and not a second time.
4. `claimRefusesAnythingWithAnOrigin` and `claimRefusesAGet`. Page script cannot reach the
   endpoint at all, and a GET neither hands out a session nor burns the code.
5. `withoutADesktopClientThereIsNoDesktopLogin`. None of it exists unless a client entity
   lists the `desktop` target.
6. `anUncollectedClaimExpires`. A code nobody collects stops standing for its session.
7. `loopbackReceiverServesOneAnswer`. The listener is loopback-only, survives the favicon
   request a browser makes beside the redirect, reflects nothing from the request into the
   page it serves, and is closed afterwards.
8. `nativeClientSignsInEndToEnd`. `Session.login()` through the real client runtime, a real
   edge and the stub provider. The edge is configured `identityRequired`, so a client that
   reaches `connected` has proved it is presenting an authenticated credential. The system
   browser is stood in for through `QDesktopServices::setUrlHandler`, Qt's own seam, and
   nothing in the client is widened to be testable.

## Staying signed in (`tst_device.cpp`, `tst_devicestore.cpp`)

`identity.desktop_session: device` lets a native app come back signed in without a browser.
What it keeps is a device credential rather than the session: redeemable once, at one
route, for a fresh session of the ordinary length. See
[desktop](../../docs/desktop.md#storing-the-session).

`tst_device.cpp` is the edge half, driven over real HTTP against a `DeviceRegistry` on a
throwaway SQLite store: enrolment issues a credential and not a session, a device secret
presented at the WebSocket upgrade is refused, every redemption rotates, a retired generation
inside the overlap window costs nothing and past it revokes the family and its sessions,
scope is re-derived through the mapping hook, both expiry clocks are enforced, an `Origin`
header is refused, logout ends the family, and an unknown secret is refused without signing
its owner out. The far side of a window measured in days is reached by moving a row's
timestamps with SQL, not by adding millisecond knobs to the config.

`tst_devicestore.cpp` is the client half. The test that matters most is the one that would
still pass if the feature were broken: with no store available, nothing is written anywhere,
asserted against the real config, data and cache directories, because there is no file
fallback and never will be. It also covers the round trip, that erasing what is not there
succeeds, that a failed write leaves nothing behind (otherwise a keyring that cannot write
would stage a theft the edge acts on), that the 2 s deadline holds, and end to end that a
second launch is still signed in and a logout stops the third.

On Linux ctest runs it through `tests/lib/keyring-session.sh`, which gives it a private
session bus and a private keyring rather than the developer's own; where those tools are
missing the store tests skip, which is what a visitor on such a machine gets. CI sets
`SYNQT_REQUIRE_SECURE_STORE` on the column that provides a store, so a skip there fails.

## The promoted auth entity (`identity.provider_entity`)

`providerEntityCentralizedLogin` and `providerEntityDistributedSessions` cover the shape
where identity is not on the edge: an auth entity owns an `identity` and a `sessions`
connect point (one Source per calling edge on both, both over mutual TLS), each edge consumes them, and the
edge holds no OAuth backend, no secret and no token; it only issues the session cookie.

Those two connect points are framework contracts, not app contracts. They live in
`src/identity/contracts/{Identity,SessionStore}.syn` and compile into `SynQtIdentity`, which is
what lets `identity.provider_entity: auth` be a single line in a project's `synqt.yaml`:
the generated auth `main.cpp` registers the Sources out of the runtime library, and no app
writes an `export:` for either. This suite hosts them exactly as that generated main does.

`auth/Identity.qml` and `auth/Session.qml` are the generator's own output, checked in here
as the fixture. `tools/synqt/synqt/authentity.py` emits them and
`tools/synqt/tests/test_provider_entity.py` compares the two, so the bridge that is proven
over a real mesh link and the bridge a project gets cannot drift apart. Edit the generator,
not these files.

`synqt add auth` is tested in `tools/synqt/tests/test_addauth.py`: the scaffolded config is
secure with no manual hardening (Authorization Code, the secret as an `env:` reference and a
`.env.example` entry, the httpOnly/SameSite session cookie, the mapping hook), and it prints
only the manual steps.

## Run

```
./run-m8.sh
```

The edge runs plaintext here because the OAuth flow is orthogonal to the public TLS. The
`Secure` cookie flag is applied under TLS (as on a real edge).
