# The base case

Goal: one item up for auction, with a current high bid that everyone sees update
live. Anyone can place a bid.

## Step 1: Declare what crosses the wire (a contract)

In SynQt, two entities talk through a connect point: a named live object that one
entity owns and others see a live copy of. You declare its shape once, in a
contract, so both sides agree on it and the compiler checks it.

Create `shared/Auction.syn`:

```syn
// The shape of the auction that the browser and the edge share.
//   prop   : a value the owner sets and consumers see update
//   slot   : a request a consumer makes; the owner decides what to do
//   signal : a message the owner sends back to consumers
contract Auction {
    prop string itemName        // what is up for auction
    prop int highBid            // the current highest bid
    prop string highBidder      // who holds the high bid right now
    slot placeBid(string bidder, int amount)
    signal bidRejected(string reason)
}
```

> [!NOTE]
> Notice the directions. Properties flow from the owner out to everyone watching.
> Slots flow the other way: a consumer asks, and the owner decides. That one
> directional trust is the whole point, and you will feel why in a moment. The
> full contract format is in [the programming model](programming-model.md).

## Step 2: Implement the owner side

The web edge will own this connect point, which means it holds the real,
authoritative auction. Create `web/Auction.qml`:

```qml
import QtQuick
import SynQt

AuctionSource {
    id: auction

    itemName: "A homemade lasagna, baked fresh this morning"
    highBid: 0
    highBidder: "nobody yet"

    // A consumer (a browser) is asking to bid. We decide whether to accept.
    function placeBid(bidder, amount) {
        if (amount <= auction.highBid) {
            Caller.emitBidRejected("Your bid must beat " + auction.highBid + ".")
            return
        }
        auction.highBid = amount
        auction.highBidder = bidder
    }
}
```

`Caller` is whoever made this request. `Caller.emitBidRejected(...)` sends the
`bidRejected` signal back to that one caller, not to everyone.

## Step 3: Wire it into the project

Tell SynQt this connect point exists, who owns it, and who may use it. Open
`synqt.yaml` and add:

```yaml
connect_points:
  - name: auction
    contract: Auction
    owner: web                # the edge holds the real auction
    consumers: [client]       # the browser may watch and bid
    server: web/Auction.qml
    instance: shared          # one auction shared by everyone
```

## Step 4: Build the UI

Open `client/Main.qml` and replace its contents:

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SynQt

ApplicationWindow {
    visible: true
    width: 480
    height: 420
    title: "Gavel"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: Server.auction.itemName
            font.pixelSize: 22
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        // These two lines update by themselves whenever the edge changes them.
        Label {
            text: "Current bid: " + Server.auction.highBid
                  + "  (held by " + Server.auction.highBidder + ")"
            font.pixelSize: 18
        }

        RowLayout {
            spacing: 8
            TextField { id: nameField; placeholderText: "Your name" }
            TextField {
                id: amountField
                placeholderText: "Amount"
                inputMethodHints: Qt.ImhDigitsOnly
            }
            Button {
                text: "Place bid"
                onClicked: {
                    Server.auction.placeBid(nameField.text, parseInt(amountField.text))
                    amountField.clear()
                }
            }
        }

        Label {
            id: errorLabel
            color: "crimson"
            visible: text.length > 0
        }

        // Listen for a rejection meant for us.
        Auction.onBidRejected: reason => errorLabel.text = reason
    }
}
```

`Server` is how the browser reaches the edge's connect points. `Server.auction` is
the live copy of the auction the edge owns.

## Step 5: Run it

Save everything and look at the browser. You should see the lasagna and a current
bid of 0. Place a bid of 50. The current bid jumps to 50 with your name.

Now the fun part. Open the same URL in a second browser tab. Bid 75 in tab two, and
watch tab one update to 75 instantly, with no refresh and no code from you to make
that happen.

> [!TIP]
> If the page is blank, check the terminal running `synqt dev` for a QML error
> (usually a typo in `Main.qml`), fix it, and save. The page reloads on its own.

## Try it, then think

> [!QUESTION]
> In tab one bid 50. In tab two bid 10. What happens to the bid of 10, and why?
> Then, predict: if you delete the line `if (amount <= auction.highBid)` from
> `web/Auction.qml` and save, what will a bid of 10 do to the standing bid of 50?

<details>
<summary>Try it, then open this</summary>

With the check in place, the bid of 10 is rejected and you see the message,
because the edge refuses any bid that does not beat the current high bid.

Delete the check, save, and bid 10 against a standing 50. It wins. The high bid
drops to 10 for everyone.

The lesson: the rule lives on the owner (the edge), and only there. The browser
never enforced it. If the only check were in the client, anyone could remove it
(it is their browser) and send any bid they liked. This is why in SynQt the owner
of a connect point is the single authority, and every rule that matters lives in
the owner's slot. Put the check back before continuing.

</details>

> [!IMPORTANT]
> Carry this with you for the rest of the tutorial: a consumer asks, the owner
> decides. Anything you must be able to trust is enforced by the owner, never by
> the consumer. Checks in the UI are only there to be friendly.

## What you learned

- A contract declares the shape of what crosses between two entities.
- A connect point is an owned, named live object; consumers see a live copy.
- Properties flow owner to consumer; slots flow consumer to owner.
- The owner is the only authority. Rules live in the owner's slots.
