<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# chat: a room everybody in it sees at once

The shortest complete SynQt system there is, and the one the front page of synqt.org reads
out file by file. Four entities, and between them every kind of thing a contract can carry.

```
browser --wss+session--> web edge --mesh mTLS--> store
gate or app              owns the room          owns the log
```

Somebody types a line, the edge appends it and reassigns one list, and every window open on
the room redraws. Nothing polls, and nobody wrote a broadcast.

## Two client bundles, not one

`gate` is what a session that has signed in as nobody is served: a sign-in button and
nothing else. `app` is the room. Which one a browser gets is `bundles:` on the edge, and it
is delivery rather than navigation -- a session without `user` cannot fetch a file of `app`
at all. Not a redirect, not a 403, simply not there.

## What crosses, and what does not

The contract carries one of each kind:

| Kind | Member | What it is |
| --- | --- | --- |
| `prop` | `topic` | The edge sets it, every window retitles itself |
| `model` | `messages` | The room's lines, mirrored into every browser |
| `slot` | `say` | The one thing a browser may ask for |
| `signal` | `refused` | What the edge says back when it says no |

`messages` carries `id`, `who` and `body`. The table also has `said_at`, which is not in the
contract, so it never leaves the mesh: a column the browser is not told about is a column it
never receives.

## Running it

```sh
synqt dev
```

Sign-in needs an OAuth app of your own: put its client id in `synqt.yaml` and its secret in
`.env` as `GITHUB_CLIENT_SECRET`. See [authentication](https://synqt.org/authentication/).

Open it on the drawing board: <https://synqt.org/designer/#example=demo>.
