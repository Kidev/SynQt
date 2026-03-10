<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# chat: a room everybody in it sees at once

The project the front page of synqt.org reads out file by file. Six entities, every kind of
thing a contract can carry, and the two decisions that make a system rather than a demo:
who is served which application, and who answers them once they are in.

```
browser ---wss+session---> web edge ---mesh mTLS---> room ---mesh mTLS---> store
home or app                a front                   or moderation         the log
```

Somebody types a line, the database appends it and reassigns one list, and every window open
on the room redraws. Nothing polls, and nobody wrote a broadcast.

## Two client bundles, not one

`home` is what a session that has signed in as nobody is served: a landing page and a
sign-in button. `app` is the room. Which one a browser gets is `bundles:` on the edge, and
it is delivery rather than navigation -- a session without `user` cannot fetch a file of
`app` at all. Not a redirect, not a 403, simply not there.

`admin` has no line in `bundles:` because scopes rank here: a moderator is served the
nearest bundle at or below what they hold, which is the room. One client for users and
moderators both.

## The edge answers none of it

The edge owns the point the browser consumes and implements no part of it. `behind:` is
what makes it a front: it keeps the session and the sign-in, and hands each caller to the
entity that serves people of their scope.

| Scope | Handed to | Which carries |
| --- | --- | --- |
| `anonymous` | nobody | the room's point is gated `scope: user`, so there is nothing to acquire |
| `user` | `room` | `topic`, `messages`, `say`, `refused` |
| `admin` | `moderation` | those, and `erase` |

That is the whole of why an ordinary user cannot erase a message: not a check somebody
wrote, but a member that is not on the surface they acquired. `synqt check` holds each
entity behind the front to exactly what the front offers its callers, so the two cannot
drift apart.

It is also why a moderator's name is red and nobody else's can be. `staff` is stamped on a
row by `moderation`, in a line that is compiled into that entity and into no other; the
browser never sends it and the entity serving ordinary users does not contain it.

## What crosses, and what does not

The browser-facing contract carries one of each kind:

| Kind | Member | What it is |
| --- | --- | --- |
| `prop` | `topic` | Set on the database, mirrored by both surfaces, relayed to every window |
| `model` | `messages` | The room's lines, mirrored into every browser |
| `slot` | `say` | The one thing any caller may ask for |
| `slot` | `erase` | Gated `<admin>`, and only on the surface a moderator acquires |
| `signal` | `refused` | What the owner says back when it says no, to the one caller who asked |

`messages` carries `id`, `who`, `body` and `staff`. The table also has `said_at`, which is
not in the contract, so it never leaves the mesh: a column the browser is not told about is
a column it cannot receive.

## Where each entity lives

| Entity | Type | File |
| --- | --- | --- |
| `home` | client | `client/home/Main.qml` |
| `app` | client | `client/app/Main.qml` |
| `edge` | web_edge | none -- it implements nothing |
| `room` | cache | `cache/room/Room.qml` |
| `moderation` | service | `service/moderation/Moderation.qml` |
| `store` | relational | `db/relational/store/Store.qml`, `schema.sql` |

## Running it

```sh
synqt dev
```

Sign-in needs an OAuth app of your own. Put its client id in `synqt.yaml` and its secret in
`.env` as `GITHUB_CLIENT_SECRET`. See [authentication](https://synqt.org/authentication/).
Who is a moderator is decided in `web/edge/identity/map.qml`, which is the one place in the
project that says so.

Open it on the drawing board: <https://synqt.org/designer/#example=demo>.
