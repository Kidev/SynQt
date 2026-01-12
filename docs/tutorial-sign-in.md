# Real bidders

There is a problem with the auction so far.

> [!CAUTION]
> Right now the bidder is only a name you type. Nothing stops you from bidding as
> "Your Boss," or as "nobody yet," or as anyone at all. A self declared name is
> worthless. To run a real auction the edge needs to know who is bidding.

Goal: people sign in, a bid is tied to their real identity, and only signed in
users can bid. Watching the auction stays open to everyone.

## Step 1: Add authentication

One command sets up secure sign in:

```cli
synqt add auth github
```

This writes an `identity` section into `synqt.yaml` with secure defaults already
on (the login flow runs on the edge, the browser never holds a secret, the session
is a hardened cookie), records the secret it needs in `.env.example`, and scaffolds
an identity mapping hook. Two things only you can do are to register the app with
GitHub, and to place the secret.

First, register a GitHub OAuth app. In a browser, go to GitHub, then Settings, then
Developer settings, then OAuth Apps, then New OAuth App. Fill in:

- Application name: anything, for example `Gavel (dev)`.
- Homepage URL: the address `synqt dev` printed, `http://127.0.0.1:8080` unless you
  passed `--port`.
- Authorization callback URL: that same address with `/auth/callback` on the end,
  `http://127.0.0.1:8080/auth/callback`. This must match exactly, `127.0.0.1` and
  all. To GitHub that is a different host from `localhost`, and a callback
  registered for one is refused for the other.

Click Register. GitHub shows a Client ID, and a button to generate a Client secret.

Second, put those two values where they belong. The Client ID is not a secret, so
it goes in `synqt.yaml`, in the provider entry `synqt add auth` created:

```yaml
      client_id: your-client-id-from-github
```

The Client secret is a secret, so it goes only in `web/edge/.env`, which is read only by
the edge and is git ignored:

```cli
GITHUB_CLIENT_SECRET=your-generated-secret
```

> [!CAUTION]
> The Client secret never goes in `synqt.yaml`, never in any file under `client/`,
> and never anywhere the browser can reach. It lives only in `web/edge/.env`, on the
> edge. SynQt will refuse to build if a secret is wired anywhere the client could
> see it, but the habit matters more than the safety net.

> [!NOTE]
> One command gives a setup that is already hardened (PKCE, a secure cookie, the
> secret kept server side) because any safety control that is optional is one
> someone eventually forgets. SynQt makes the secure path the default path,
> with no working but insecure middle state to get stuck in. If you want the full
> picture of what was turned on for you, see [authentication](authentication.md).

## Step 2: Use the real identity instead of a typed name

Now that the edge knows who the caller is, the bidder should come from their
identity rather than a text field. Change the edge's `export:` in `synqt.yaml`, and say on the
member itself who may reach it:

```yaml
      <user> slot placeBid(int amount)   // signed-in users only, and the edge knows who you are
```

The `<user>` is the gate. A caller who does not hold that scope does not have the member.
The call is refused before your function runs, so there is no check to write in the QML,
and none to forget. Update `web/edge/Edge.qml` to use the caller's identity:

```qml
function placeBid(amount) {
    if (amount <= auction.highBid) {
        Caller.emitBidRejected("Your bid must beat " + auction.highBid + ".")
        return
    }
    auction.highBid = amount
    auction.highBidder = Caller.identity.name   // their real name, from sign in
}
```

> [!NOTE]
> Scopes are the permission levels of your app (anonymous, user, moderator, admin by
> default), and `<user>` on a member means "at least a signed in user". What is left in
> the function is the decision the topology cannot make, whether this particular bid is
> good enough. `Caller.identity` is the authenticated profile. The bidder cannot type it,
> because it comes from the login they completed.

## Step 3: Update the UI for sign in

Replace the bidding row and add sign in to `client/app/Main.qml`. The view now shows a
Sign in button when you are anonymous, and the bid controls only when you are
signed in:

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
            Server.placeBid(parseInt(amountField.text))
            amountField.clear()
        }
    }
}
```

`Session` is the browser's read only view of who you are. `Session.login()` starts
the sign in flow, and `Session.hasScope("user")` is true once you are signed in.

## Step 4: Run it

Save and look at the browser. You now see "Sign in to bid." Click it, complete the
GitHub login, and you return signed in, with your real name shown and the bid box
available. Bid, and your real name holds the high bid.

## Try it, then think

> [!QUESTION]
> Is hiding the bid controls enough to keep signed out people from bidding? Sign
> out (or open a private window), then open your browser developer console and run,
> by hand:
>
> ```
> Server.placeBid(999)
> ```
>
> Predict what happens before you press Enter.

<details class="solution" markdown>
<summary>Solution</summary>

The bid is rejected. You see nothing change at all. The standing bid is where it was.

Hiding the controls only removed the button from view. A determined visitor can
still call the slot directly, as you just did. What stopped the bid was the
`<user>` gate on `placeBid`, which the edge applies before the function runs.

This is the same lesson as [the base case](tutorial-base-auction.md), now for
permissions. Authorization happens on
the owner, against `Caller`, every time. The client showing or hiding a control is
never the security boundary. SynQt's whole security model rests on this, and it is
laid out in [security](security.md).

</details>

## Bonus: an auctioneer who can close a lot

One person, the auctioneer, gets the power to close the current lot and put up the
next one. This shows a higher permission level (admin).

Add to the edge's `export:`, gated a level higher:

```yaml
      <admin> slot closeLot(string[120] nextItem)
```

Who may do it is already settled by the `<admin>` on the member, so the function in
`web/edge/Edge.qml` only has to do the work:

```qml
function closeLot(nextItem) {
    // (A later part records the winner here before resetting.)
    auction.itemName = nextItem
    auction.highBid = 0
    auction.highBidder = "nobody yet"
}
```

Make yourself the auctioneer by mapping your identity to the admin scope. Open
`web/edge/identity/map.qml` (scaffolded by `synqt add auth`) and return `"admin"` for
your own account:

```qml
import QtQuick
import SynQt

IdentityMapping {
    function scopeFor(identity) {
        const auctioneers = ["you@example.com"]   // your GitHub email
        if (auctioneers.indexOf(identity.email) !== -1) return "admin"
        return "user"   // everyone else who signs in
    }
}
```

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
> `identity.email` can be null, because a GitHub account set to keep its email private may
> expose no address even after sign in. For a mapping that always works, key on
> `identity.login` (the GitHub username) or `identity.sub` (the stable id) instead,
> as the multiplayer tutorial's guest list does. The identity fields are defined in
> [authentication](authentication.md#the-identity-object).

Sign in as yourself and you can close the lot and start the next one. Anyone else
who tries (or who calls `closeLot` from the console) is refused by the edge.

## What you learned

- `synqt add auth` gives you secure sign in in one step, with no insecure state.
- Identity comes from a real login, through `Caller.identity`, and cannot be faked
  by the caller.
- Authorization is per member, written on the member as `<scope>`, and applied by the
  edge before your code runs. What stays in the slot is the judgement the topology cannot
  make, against `Caller`.
- Scopes are permission levels. An admin can do what a user cannot.
- Hiding controls in the UI is courtesy rather than security.
