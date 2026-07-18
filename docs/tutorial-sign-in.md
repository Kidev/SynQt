# Real bidders

There is a problem with our auction, and you may have already felt it.

> [!CAUTION]
> Right now the bidder is just a name you type. Nothing stops you from bidding as
> "Your Boss," or as "nobody yet," or as anyone at all. A self declared name is
> worthless. To run a real auction we need to know who is actually bidding.

Goal: people sign in, a bid is tied to their real identity, and only signed in
users can bid. Watching the auction stays open to everyone.

## Step 1: Add authentication

One command sets up secure sign in:

```cli
synqt add auth github
```

This writes an `identity` section into `synqt.yaml` with secure defaults already
on (the login flow runs on the edge, the browser never holds a secret, the session
is a hardened cookie), adds `web/.env.example`, and scaffolds an identity mapping
hook. Two things only you can do: register the app with GitHub, and place the
secret. Here they are, concretely.

First, register a GitHub OAuth app. In a browser, go to GitHub, then Settings, then
Developer settings, then OAuth Apps, then New OAuth App. Fill in:

- Application name: anything, for example `Gavel (dev)`.
- Homepage URL: the address `synqt dev` printed, for example `http://localhost:8000`.
- Authorization callback URL: that same address with `/auth/callback` on the end,
  for example `http://localhost:8000/auth/callback`. This must match exactly.

Click Register. GitHub shows a Client ID, and a button to generate a Client secret.

Second, put those two values where they belong. The Client ID is not a secret, so
it goes in `synqt.yaml`, in the provider entry `synqt add auth` created:

```yaml
      client_id: your-client-id-from-github
```

The Client secret is a secret, so it goes only in `web/.env`, which is read only by
the edge and is git ignored:

```cli
GITHUB_CLIENT_SECRET=your-generated-secret
```

> [!CAUTION]
> The Client secret never goes in `synqt.yaml`, never in any file under `client/`,
> and never anywhere the browser can reach. It lives only in `web/.env`, on the
> edge. SynQt will refuse to build if a secret is wired anywhere the client could
> see it, but the habit matters more than the safety net.

> [!NOTE]
> Why does one command give you a setup that is already hardened (PKCE, a secure
> cookie, the secret kept server side)? Because any safety control that is optional
> is one someone eventually forgets. SynQt makes the secure path the default path,
> with no working but insecure middle state to get stuck in. If you want the full
> picture of what was turned on for you, see [authentication](authentication.md).

## Step 2: Use the real identity, not a typed name

Now that the edge knows who the caller is, the bidder should come from their
identity, not a text field. Change the contract in `shared/Auction.syn`:

```syn
slot placeBid(int amount)   // no more bidder argument; the edge knows who you are
```

Update `web/Auction.qml` to authorize the user and use their identity:

```qml
function placeBid(amount) {
    // Only signed in users may bid.
    if (!Caller.hasScope("user")) {
        Caller.emitBidRejected("Please sign in to bid.")
        return
    }
    if (amount <= auction.highBid) {
        Caller.emitBidRejected("Your bid must beat " + auction.highBid + ".")
        return
    }
    auction.highBid = amount
    auction.highBidder = Caller.identity.name   // their real name, from sign in
}
```

> [!NOTE]
> `Caller.hasScope("user")` asks whether the caller is at least a signed in user.
> Scopes are the permission levels of your app (anonymous, user, moderator, admin
> by default). `Caller.identity` is the authenticated profile: it cannot be typed
> by the bidder, it comes from the login they actually completed.

## Step 3: Update the UI for sign in

Replace the bidding row and add sign in to `client/Main.qml`. The view now shows a
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
            Server.auction.placeBid(parseInt(amountField.text))
            amountField.clear()
        }
    }
}
```

`Session` is the browser's read only view of who you are. `Session.login()` starts
the sign in flow; `Session.hasScope("user")` is true once you are signed in.

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
> Server.auction.placeBid(999)
> ```
>
> Predict what happens before you press Enter.

<details>
<summary>Try it, then open this</summary>

The bid is rejected. You see nothing change, and if you were listening you would
get "Please sign in to bid."

Hiding the controls only removed the button from view. A determined visitor can
still call the slot directly, as you just did. What actually stopped the bid was
the `Caller.hasScope("user")` check inside the edge's `placeBid`. The UI visibility
was a courtesy; the edge was the guard.

This is the same lesson as [the base case](tutorial-base-auction.md), now for
permissions: authorization happens on
the owner, against `Caller`, every time. The client showing or hiding a control is
never the security boundary. SynQt's whole security model rests on this, and it is
laid out in [security](security.md).

</details>

## Bonus: an auctioneer who can close a lot

Let us give one person, the auctioneer, the power to close the current lot and put
up the next one. This shows a higher permission level (admin).

Add to `shared/Auction.syn`:

```syn
slot closeLot(string nextItem)
```

Add to `web/Auction.qml`:

```qml
function closeLot(nextItem) {
    if (!Caller.hasScope("admin")) {
        Caller.emitBidRejected("Only the auctioneer can close a lot.")
        return
    }
    // (A later part records the winner here before resetting.)
    auction.itemName = nextItem
    auction.highBid = 0
    auction.highBidder = "nobody yet"
}
```

Make yourself the auctioneer by mapping your identity to the admin scope. Open
`web/identity/map.qml` (scaffolded by `synqt add auth`) and return `"admin"` for
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

Add an auctioneer control to `client/Main.qml`, visible only to admins:

```qml
RowLayout {
    spacing: 8
    visible: Session.hasScope("admin")
    TextField { id: nextItemField; placeholderText: "Next item" }
    Button {
        text: "Close lot"
        onClicked: Server.auction.closeLot(nextItemField.text)
    }
}
```

> [!NOTE]
> `identity.email` can be null: a GitHub account set to keep its email private may
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
- Authorization is per action, in the owner's slot, with `Caller.hasScope(...)`.
- Scopes are permission levels; an admin can do what a user cannot.
- Hiding controls in the UI is courtesy, not security.
