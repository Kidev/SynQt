<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Developing locally

`synqt dev` runs the whole system on your machine, and it carries a few helpers that exist
only there. Most of them are about identity: being signed in without an OAuth app, being a
moderator for a minute, being a particular person again after a restart, and being two
people at once in one browser. This page lists them side by side, says which one to use
when, and says why none of them can reach a deployed system.

| You want to | Use | What runs |
| --- | --- | --- |
| Sign in before there is an OAuth app to register | the [development sign-in](#signing-in-with-no-provider-identitydev_stub) (`identity.dev_stub`) | the whole login flow against a provider that runs inside the edge |
| See what a scope sees, right now | the [scope picker](#skipping-the-flow-synqt-dev-identity-picker) (`synqt dev --identity-picker`) | no flow at all. A session is minted at the scope you click |
| Be a named person, the same one every run | [named people](#being-somebody-in-particular-dev-identities) (a `.dev-identities` file) | the picker, and your own mapping hook deciding their scope |
| Watch two people interact in one browser | [this tab only](#two-people-in-one-browser) on the picker | one session per tab, under a cookie of its own |

## What `synqt dev` does for you

Besides building and starting every entity:

- **A throwaway mesh authority.** The first run creates a development CA and a certificate
  per entity, so every link between entities is mutual TLS from the first minute and there
  is nothing to switch off. It is kept apart from any production CA. See
  [the mesh certificate tooling](build-system-and-cli.md#the-mesh-certificate-tooling).
- **Plaintext on loopback for the browser.** The edge serves the client over HTTP bound to
  localhost, so there is no certificate to accept. `synqt build --release` and `synqt serve`
  refuse an edge that has neither a `tls:` block nor a named upstream that terminates TLS.
- **Reload on save.** Client QML rebuilds and reloads the tab, a contract change regenerates
  the contract layer and rebuilds every entity that uses it, service QML reloads its entity,
  and an [edge-delivered page](remote-pages.md) is pushed with no rebuild. `synqt dev
  --desktop` does the same for a native window.

## Signing in with no provider: `identity.dev_stub`

```cli
synqt add auth dev
```

writes a development provider into `synqt.yaml`:

```yaml
identity:
  dev_stub:
    users:
      - { sub: dev, login: dev, name: Developer, email: dev@localhost }
      - { sub: mod, login: mod, name: Moderator, email: moderator@localhost }
```

`Session.login()` then goes through the real flow (the state, PKCE, the code exchange, the
ID token and its signature check, your mapping hook, the httpOnly session cookie) against a
provider the edge runs on loopback. With more than one user listed, the sign-in asks which
of them you are. Each one is an identity rather than a scope, so what they can reach is
whatever your mapping hook returns for them. To test a moderator, list somebody your hook
maps to `moderator`.

Use it when the question is about signing in: the redirect, the callback, the cookie, or
what your hook makes of an identity. The full description is under
[the development sign-in](authentication.md#the-development-sign-in).

## Skipping the flow: `synqt dev --identity-picker`

```cli
synqt dev --identity-picker
```

replaces every sign-in the project has with one page at `/synqt/dev/identity` listing the
scopes in `scopes.order`. Click one and you hold a session at that scope, with a made up
identity whose `sub` is `synqt-dev:<scope>:<epoch-ms>`, which no real provider can issue.

OAuth and the mapping hook both stay out of it, because the point is to pick the scope
directly. A posted scope is still checked against `scopes.order`, so editing the form cannot
mint a scope the project never declared.

Use it when the question is what a scope can see or do. See
[the scope picker](authentication.md#skipping-the-flow-the-scope-picker).

## Being somebody in particular: `.dev-identities`

Anything keyed to a person (their rows, their name above their head, the things only the
author may delete) needs the same person back after a restart, and a picked scope is a new
identity every time. A `.dev-identities` file at the project root lists the people instead:

```yaml
- email: alice@example.com
  scope: admin
- email: bob@example.com
  scope: user
```

The picker offers each of them beside the scopes, and clicking one signs you in as that
person. Their `sub` is `synqt-dev:<email>`, the same on every run, their `login` is the
part of the address before the `@`, and their `name` is the whole address.

This mode does ask your mapping hook. When the hook disagrees with the file, the page shows
both and the session gets the hook's answer. When the hook refuses the identity, the picker
refuses it too, so development cannot reach a state your own rule forbids.

`synqt dev` reads the file, drops an entry that names an undeclared scope or misses a field
(and says so on the page and in the terminal), and adds `.dev-identities` to `.gitignore`
the first time it sees one. The addresses belong to the people on one machine. See
[being somebody in particular](authentication.md#being-somebody-in-particular-dev-identities).

## Two people in one browser

Tick **this tab only** before choosing a scope or a person, and the session belongs to that
tab alone. Open a second tab, choose somebody else, and the two tabs are two people: a
moderator erasing the line a user is reading, or two players walking into each other, with
no second browser profile and no private window.

Two tabs on one host normally share every cookie, so the picker gives the tab its own
cookie name, `synqt_session_<nonce>`, and sends it to `/?s=<nonce>` so the edge knows which
cookie is that tab's. The nonce only names the cookie. The session id inside it is still the
credential. See [two tabs, two people](authentication.md#two-tabs-two-people).

## None of it ships

A development sign-in in a deployed system lets anybody in, so a release build leaves
these helpers out entirely:

- The sources are compiled only under `SYNQT_DEV_TOOLS`, which `synqt dev` sets and
  `synqt build` never does.
- Their headers carry an `#error` that stops a release build that includes one.
- The generated edge `main.cpp` names them only in a development build.
- At run time, the stub server starts only under `--dev`, the edge refuses the stub provider
  entry without it, and the picker's routes exist only under `--identity-picker`.

[`tests/dev-exclusion`](https://github.com/Kidev/SynQt/tree/main/tests/dev-exclusion)
builds the framework both ways and reads the symbol tables to prove the release one holds
none of it. Leaving `dev_stub` in `synqt.yaml` is fine: `synqt check --release` notes that
the project carries one and that it is inert.
[Development code cannot ship](security.md#development-code-cannot-ship)
has the full reasoning.
