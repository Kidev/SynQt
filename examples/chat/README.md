<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# chat: a room everybody in it sees at once

The project the front page of synqt.org reads out file by file. Three entities, four
files, and the whole model in miniature: an owner that decides, a consumer that asks, a
contract that says what may cross, and a database the browser has no way to reach.

```
browser ---wss+session---> web edge ---mesh mTLS---> store
                           owns the room             the log
```

Somebody types a line, the database appends it and reassigns one list, and every window open
on the room redraws. Nothing polls, and nobody wrote a broadcast.

## Two gates, neither of them in the client

| Gate | Where it is written | What it does |
| --- | --- | --- |
| `scope: user` | on the connect point | a session that has signed in as nobody never acquires it, so there is no room to reach and no sign-in screen to get past |
| `<admin> slot erase` | on the member | an ordinary user does not have the slot; a console call finds nothing to call |

The client hides the sign-in page once you are in, and it shows the Erase button to a
moderator. Both are conveniences. The edge is what stops anybody.

## What crosses, and what does not

| Kind | Member | What it is |
| --- | --- | --- |
| `model` | `messages` | the room's lines, mirrored into every browser |
| `slot` | `say` | the one thing any signed-in caller may ask for |
| `slot` | `erase` | gated `<admin>`, and the line stays and says it is gone |

The browser sends a line of text and nothing else. Who said it and whether they said it as
staff are decided by the edge, from the session it verified, so an ordinary user cannot
speak in a moderator's voice by asking to.

`messages` carries `id`, `who`, `body` and `staff`. The table also has `said_at`, which
is in no contract, so it never leaves the mesh. The browser cannot receive a column it is
not told about.

## Where each entity lives

| Entity | Type | File |
| --- | --- | --- |
| `app` | client | `client/app/Main.qml`, `User.qml`, `Admin.qml` |
| `edge` | web_edge | `web/edge/Edge.qml` |
| `store` | relational | `db/relational/store/Store.qml`, `schema.sql` |

## Running it

```sh
synqt dev
```

Sign-in needs an OAuth app of your own. Put its client id in `synqt.yaml` and its secret
in `.env` as `GITHUB_CLIENT_SECRET`. See
[authentication](https://synqt.org/authentication/). `web/edge/identity/map.qml` decides
who is a moderator, and it is the one place in the project that says so.

Open it on the drawing board: <https://synqt.org/designer/#example=demo>.
