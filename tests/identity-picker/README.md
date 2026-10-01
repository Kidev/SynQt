<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# tests/identity-picker

The development scope picker, in a real browser, with a real cookie jar.

```bash
bash tests/identity-picker/run-identity-picker.sh
```

## What only a browser can say

`tests/webedge` already drives the picker's routes. It proves the page lists the
project's declared scopes and nothing else, that a posted index is bounds-checked, that a
per-tab choice answers `303` with `Location: /?s=<nonce>` and a `synqt_session_<nonce>`
cookie, and that the routes do not exist at all in an edge nobody asked for the picker.

What it cannot say is what a browser does with two of those cookies. Two tabs of one
browser context share one jar, because RFC 6265 scopes a cookie to a host and not a port,
and "the second sign-in did not become the first" is a statement about that jar. This
suite makes that statement. It opens three tabs of one context, signs a moderator in on
one and a user in on another, and checks that the first is still a moderator afterwards.

Reverting the cookie-name resolution (`WebEdge::cookieNameFor`) to the fixed name makes it
fail, and the failure is the one the mechanism exists to prevent:

```
FAIL chromium: tab one became user when tab two signed in
```

## How a tab is asked who it is

The session cookie is httpOnly, so a page cannot read it. The edge answers `/` with the
bundle mapped to the caller's scope, so the project written by `make-project.py` maps each
of its three scopes to a directory holding one line of HTML. Which line came back is the
answer.

The suite needs no WebAssembly kit and builds no client. Three static directories answer
that question exactly as a compiled client would, and the client this project declares
exists only so the topology is the ordinary one.

## What else it covers

- The page offers the named identities from `.dev-identities` beside the scopes, and it
  reports an entry naming a scope the project does not declare instead of failing.
- The ordinary shared sign-in still works and leaves the per-tab sessions alone, which is
  the half a per-tab mechanism is most likely to break.
