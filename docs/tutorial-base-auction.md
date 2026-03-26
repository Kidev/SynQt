<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The base case

Goal: one item up for auction, with a high bid that everyone sees update live. Anyone
can bid.

## Step 1: Declare what crosses the wire (a connect point)

Two entities talk through a connect point: a live object that one entity owns and the
others mirror. You declare it once, in `synqt.yaml`: who owns it, who may use it, and the
shape of what crosses it. Both sides share that shape, and the compiler checks it.

Open `synqt.yaml` and add:

```yaml
connect_points:
  - owner: edge               # the edge holds the real auction
    consumers: [app]          # the browser may watch and bid
    # The shape of the auction that the browser and the edge share.
    #   prop   : a value the owner sets and consumers see update
    #   slot   : a request a consumer makes, and the owner decides what to do
    #   signal : a message the owner sends back to consumers
    export: |
      prop string[120] itemName   // what is up for auction
      prop int highBid            // the current highest bid
      prop string[80] highBidder  // who holds the high bid right now
      slot placeBid(string[80] bidder, int amount)
      signal bidRejected(string[120] reason)
```

The owner is `edge`, so the type it exports is `Edge`. You write that name in QML in a
moment.

> [!NOTE]
> Properties flow from the owner to everyone watching. Slots flow the other way: a
> consumer asks, and the owner decides. The exercise after step 4 shows why that direction
> matters. The full contract format, and the sizes in the brackets, are in
> [the programming model](programming-model.md#the-types-a-contract-can-name).

Once the edge implements these properties (step 2), you could export them by name alone:
`synqt` reads their types from the owner, and `synqt check` refuses a name the owner does
not have. Writing the type out is never wrong, and it is the only way to bound a type,
so the tutorial writes them out. See
[exporting by name](programming-model.md#exporting-by-name).

## Step 2: Implement the owner side

The web edge owns the connect point, so it runs the auction. There is one lot, however
many people watch, and one file holds it: `web/edge/Edge.qml`, which `synqt new` already
wrote. Open it and add the auction.

```qml
import SynQt

Edge {
    id: auction

    itemName: "A homemade lasagna, baked fresh this morning"
    highBid: 0
    highBidder: "nobody yet"

    // A consumer (a browser) is asking to bid. The edge decides whether to accept.
    function placeBid(bidder, amount) {
        if (amount <= auction.highBid) {
            Caller.emitBidRejected("Your bid must beat " + auction.highBid + ".");
            return;
        }
        auction.highBid = amount;
        auction.highBidder = bidder;
    }
}
```

The `export:` block declared these three properties, so setting them here publishes
them. Every browser watching sees the new value; you write nothing else.

`Caller` is whoever made this request. `Caller.emitBidRejected(...)` sends the
`bidRejected` signal to that caller only, not to everyone.

The edge is shared (the default for every entity but the client), so one Source holds the
one lot. Each caller still arrives with their own `Caller`, so the rejection goes back
only to the browser that bid too low.

## Step 3: Build the UI

Open `client/app/Main.qml` and replace its contents:

```qml
import SynQt
import QtQuick.Controls
import QtQuick.Layouts

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
            text: Server.itemName
            font.pixelSize: 22
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        // These two lines update by themselves whenever the edge changes them.
        Label {
            text: "Current bid: " + Server.highBid
                  + "  (held by " + Server.highBidder + ")"
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
                    Server.placeBid(nameField.text, parseInt(amountField.text));
                    amountField.clear();
                }
            }
        }

        Label {
            id: errorLabel
            color: "crimson"
            visible: text.length > 0
        }

        // Listen for a rejection meant for this browser.
        Edge.onBidRejected: reason => errorLabel.text = reason
    }
}
```

`Server` is the browser's handle on the edge's connect point. `Server.itemName`,
`Server.highBid` and `Server.highBidder` are live copies of what the edge owns.

## Step 4: Run it

Save everything and look at the browser. You see the lasagna and a current bid of 0.
Bid 50: the current bid jumps to 50, with your name.

Open the same URL in a second tab and bid 75 there. Tab one shows 75 at once, with no
refresh and no code of yours.

> [!TIP]
> If the page is blank, check the terminal running `synqt dev` for a QML error
> (usually a typo in `Main.qml`), fix it, and save. The page reloads on its own.

## Try it, then think

> [!QUESTION]
> In tab one bid 50. In tab two bid 10. What happens to the bid of 10, and why?
> Then, predict: if you delete the line `if (amount <= auction.highBid)` from
> `web/edge/Edge.qml` and save, what will a bid of 10 do to the standing bid of 50?

<details class="solution" markdown>
<summary>Solution</summary>

With the check in place, the edge rejects the bid of 10 and you see the message: the
edge refuses any bid that does not beat the high bid.

Delete the check, save, and bid 10 against a standing 50. It wins, and the high bid drops
to 10 for everyone.

The rule lives on the owner (the edge) and nowhere else; the browser never enforced it.
If the only check were in the client, anyone could remove it (it is their browser) and
send any bid. So the owner of a connect point is the single authority, and every rule
that matters lives in the owner's slot. Put the check back before you continue.

</details>

> [!IMPORTANT]
> Remember this for the rest of the tutorial: a consumer asks, and the owner decides.
> The owner enforces anything you must be able to trust, never the consumer. Checks in
> the UI only make it friendlier.

## What you learned

- A contract declares the shape of what crosses between two entities.
- A connect point is a live object with one owner, and consumers see a live copy.
- Properties flow owner to consumer, and slots flow consumer to owner.
- The owner is the only authority. Rules live in the owner's slots.
