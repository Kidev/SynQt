<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M8: identity and easy auth

The whole OAuth2 / OpenID Connect Authorization Code flow runs on the web edge; the
browser only ever ends with an httpOnly session cookie.

## What runs

- **`IdentityProvider`** (`src/service`); `QOAuth2AuthorizationCodeFlow` with PKCE, a
  **framework-generated** state verified on the callback (a state the edge did not issue is
  refused before any token exchange), the token exchange with the edge-held
  `client_secret`, identity normalization per `docs/authentication.md`, the scope-mapping
  QML hook (`IdentityMapping.scopeFor`), the authenticated session, and the
  `HttpOnly`/`Secure`/`SameSite` cookie. A custom `EdgeReplyHandler` gives the flow the
  edge's **public** callback URL instead of a loopback port.
- **`JwksVerifier`** (`src/service`); for OpenID Connect providers, verifies the ID token's
  RS256 signature against the provider JWKS (fetched and cached with `QNetworkAccessManager`)
  plus iss/aud/exp/nonce, using pinned **jwt-cpp** (MIT, vcpkg). No hand-rolled crypto; the
  signature is checked with the no-throw `rs256::verify(..., ec)` so no exception crosses the
  boundary.
- **`StubIdentityServer`** (`src/service`); a dev-only provider (`/authorize`, `/token`,
  `/userinfo`, `/jwks`) that authenticates a preconfigured user, verifies the PKCE S256
  verifier and the client secret, and issues a real RS256-signed ID token. It is gated so it
  can never ship: it takes a `DevOnly` acknowledgement, and the runtime refuses a `devStub`
  provider unless `identity.allow_dev_stub` is on (only `synqt dev` sets it).

## What the tests check (`tst_m8.cpp`)

1. **fullLoginFlow**: login -> provider -> callback creates a session; the cookie is
   `HttpOnly`; the session carries the normalized identity and the scope the map hook
   returned (`moderator` for octocat); the access token is held on the edge, is not the
   cookie value, and appears in neither the `Set-Cookie` nor the response body.
2. **oidcLoginVerifiesIdToken**: an OIDC provider with **no** userinfo endpoint: a session
   is created only because the ID token's signature verified against the JWKS; the identity
   comes from the ID-token claims and the auth request carried a nonce.
3. **oidcWrongIssuerRejected**: an ID token whose `iss` does not match fails verification;
   no session.
4. **unknownStateRejected**: a forged state is refused (400) before any token exchange.
5. **devStubRefusedWithoutGate**: with the dev gate off, the dev stub provider is refused
   (403).

`synqt add auth` is tested in `tools/synqt/tests/test_addauth.py`: the scaffolded config is
secure with no manual hardening (Authorization Code, the secret as an `env:` reference and a
`.env.example` entry, the httpOnly/SameSite session cookie, the mapping hook), and it prints
only the manual steps.

## Run

```
./run-m8.sh
```

The edge runs plaintext here because the OAuth flow is orthogonal to the public TLS; the
`Secure` cookie flag is applied under TLS (as on a real edge).
