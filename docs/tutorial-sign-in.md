<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Real bidders

> [!CAUTION]
> So far the bidder is just a name you type. You can bid as "Your Boss", as "nobody
> yet", or as anyone else, so the name proves nothing. A real auction
> needs the edge to know who is bidding.

Goal: people sign in, each bid carries their real identity, and only signed-in users can
bid. Anyone can still watch.

## Step 1: Add authentication

One command sets up secure sign-in:

```cli
synqt add auth github
```

It writes an `identity` section into `synqt.yaml` with secure defaults (the login flow
runs on the edge, the browser never holds a secret, the session is a hardened cookie),
lists the secret it needs in `.env.example`, and scaffolds an identity mapping hook. Two
steps are yours: register the app with GitHub, and store the secret.

First, register a GitHub OAuth app. On GitHub, open Settings, then Developer settings,
then OAuth Apps, then New OAuth App. Fill in:

- Application name: anything, for example `Gavel (dev)`.
- Homepage URL: the address `synqt dev` printed, `http://127.0.0.1:8080` unless you
  passed `--port`.
- Authorization callback URL: the same address followed by `/auth/callback`, so
  `http://127.0.0.1:8080/auth/callback`. It must match exactly, `127.0.0.1` included:
  GitHub treats `localhost` as a different host and refuses a callback registered for
  the other one.

Click Register. GitHub shows a Client ID and a button that generates a Client secret.

Second, store the two values. The Client ID is not secret, so it goes in `synqt.yaml`,
in the provider entry `synqt add auth` created:

```yaml
      client_id: your-client-id-from-github
```

The Client secret goes only in `web/edge/.env`, which only the edge reads and git
ignores:

```text
GITHUB_CLIENT_SECRET=your-generated-secret
```

> [!CAUTION]
> The Client secret never goes in `synqt.yaml`, in any file under `client/`, or anywhere
> the browser can reach. It lives only in `web/edge/.env`, on the edge. SynQt refuses to
> build if a secret is wired anywhere the client could see it, but do not rely on that
> safety net.

> [!NOTE]
> The command turns on every protection at once (PKCE, a secure cookie, the secret
> kept on the server), because someone eventually forgets an optional safety control.
> Every setting that works is also secure. For everything it turned on, see
> [authentication](authentication.md).

## Step 2: Use the real identity instead of a typed name

Now the edge knows who is calling, so the bidder should come from that identity, not
from a text field. Change the edge's `export:` in `synqt.yaml`, and state on the member
who may reach it:

```yaml
      <user> slot placeBid(int amount)   // signed-in users only, and the edge knows who you are
```

`<user>` is the gate. A caller without that scope does not have the member, and the edge
refuses the call before your function runs, so you write no check in the QML and cannot
forget one. Update `web/edge/Edge.qml` to use the caller's identity:

```qml
function placeBid(amount) {
    if (amount <= auction.highBid) {
        Caller.emitBidRejected("Your bid must beat " + auction.highBid + ".");
        return;
    }
    auction.highBid = amount;
    auction.highBidder = Caller.identity.name;   // their real name, from sign in
}
```

> [!NOTE]
> Scopes are your app's permission levels (by default anonymous, user, moderator,
> admin), and `<user>` on a member means "at least a signed-in user". The function keeps
> only the decision the topology cannot make: whether this bid is high enough.
> `Caller.identity` is the authenticated profile. It comes from the login the bidder
> completed, so they cannot type it.

## Step 3: Update the UI for sign in

In `client/app/Main.qml`, replace the bidding row and add sign-in. The view shows a Sign
in button to anonymous visitors and the bid controls only to signed-in users:

```qml
RowLayout {
    spacing: 8
    visible: !Session.hasScope("user")
    Button {
        text: "Sign in to bid"
        onClicked: Session.login()
    }
}

RowLayout {
    spacing: 8
    visible: Session.hasScope("user")
    Label { text: "Signed in as " + (Session.identity ? Session.identity.name : "") }
    TextField {
        id: amountField
        placeholderText: "Amount"
        inputMethodHints: Qt.ImhDigitsOnly
    }
    Button {
        text: "Place bid"
        onClicked: {
            Server.placeBid(parseInt(amountField.text));
            amountField.clear();
        }
    }
}
```

`Session` is the browser's read only view of who you are. `Session.login()` starts the
sign-in flow, and `Session.hasScope("user")` is true once you are signed in.

## Step 4: Run it

Save and look at the browser. It shows "Sign in to bid". Click it and complete the
GitHub login. You come back signed in, with your name shown and the bid box available.
Bid, and your real name holds the high bid.

## Try it, then think

> [!QUESTION]
> Does hiding the bid controls stop signed-out people from bidding? Sign out (or open a
> private window), open the browser's developer console, and run:
>
> ```
> Server.placeBid(999)
> ```
>
> Predict what happens before you press Enter.

<details class="solution" markdown>
<summary>Solution</summary>

The edge rejects the bid, and the standing bid stays where it was.

Hiding the controls only removed the button from view. A visitor can still call the
slot directly, as you just did. The `<user>` gate on `placeBid` stopped the bid; the edge
applies it before the function runs.

This is the lesson of [the base case](tutorial-base-auction.md) again, for permissions:
the owner authorizes every call, against `Caller`. Showing or hiding a control on the
client is never the security boundary. SynQt's security model rests on this; see
[security](security.md).

</details>

## Bonus: an auctioneer who can close a lot

Give one person, the auctioneer, the power to close the current lot and put up the next
one. This uses a higher permission level, admin.

Add this to the edge's `export:`, gated one level higher:

```yaml
      <admin> slot closeLot(string[120] nextItem)
```

The `<admin>` on the member already decides who may call it, so the function in
`web/edge/Edge.qml` only does the work:

```qml
function closeLot(nextItem) {
    // (A later part records the winner here before resetting.)
    auction.itemName = nextItem;
    auction.highBid = 0;
    auction.highBidder = "nobody yet";
}
```

Make yourself the auctioneer by mapping your identity to the admin scope. Open
`web/edge/identity/map.qml` (scaffolded by `synqt add auth`) and return `Scope.Admin`
for your own account:

```qml
import SynQt

IdentityMapping {
    readonly property var auctioneers: ["your-github-username"]

    function scopeFor(identity): int {
        if (auctioneers.indexOf(identity.login) !== -1) {
            return Scope.Admin;
        }
        return Scope.User;   // everyone else who signs in
    }
}
```

`Scope` is generated from `scopes.order` in your `synqt.yaml`, so `Admin` exists because
`admin` is declared there. If you misspell it, `synqt check` names the missing member and
lists the valid ones, so you do not find out when somebody signs in and can reach
nothing.

Add an auctioneer control to `client/app/Main.qml`, visible only to admins:

```qml
RowLayout {
    spacing: 8
    visible: Session.hasScope("admin")
    TextField { id: nextItemField; placeholderText: "Next item" }
    Button {
        text: "Close lot"
        onClicked: Server.closeLot(nextItemField.text)
    }
}
```

> [!NOTE]
> The mapping keys on `identity.login` (the GitHub username). `identity.sub` (the stable
> id) works too. `identity.email` does not: a GitHub account that keeps its email private
> may expose no address even after sign-in. The identity fields are defined in
> [authentication](authentication.md#the-identity-object).

Sign in as yourself and you can close the lot and start the next one. The edge refuses
anyone else who tries, including from the console with `closeLot`.

## What you learned

- `synqt add auth` sets up secure sign-in in one step, with no insecure setting.
- Identity comes from a real login, through `Caller.identity`, and the caller cannot
  fake it.
- Authorization is per member: you write `<scope>` on the member, and the edge applies it
  before your code runs. The slot keeps only the judgement the topology cannot make,
  against `Caller`.
- Scopes are permission levels. An admin can do what a user cannot.
- Hiding controls in the UI is a courtesy, not security.
