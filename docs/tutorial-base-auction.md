# The base case

Goal: one item up for auction, with a current high bid that everyone sees update
live. Anyone can place a bid.

## Step 1: Declare what crosses the wire (a connect point)

In SynQt, two entities talk through a connect point, a named live object that one
entity owns and others see a live copy of. You declare it once, in `synqt.yaml`, with who
owns it, who may use it, and the shape of what crosses it. Both sides agree on that shape and
the compiler checks it.

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

The owner is `edge`, so the type it exports is `Edge`. That is the name you will
write in QML in a moment, and nothing else names it.

> [!NOTE]
> Properties flow from the owner out to everyone watching.
> Slots flow the other way. A consumer asks, and the owner decides. Step 4 shows why
> that direction matters. The
> full contract format, and the sizes in those brackets, are in
> [the programming model](programming-model.md#the-types-a-contract-can-name).

Once the edge implements this, in step 3, those three properties can be exported by name
alone. `synqt` reads what they are from the owner, and `synqt check` refuses a name the
owner does not have. Written out is never wrong, and it is the only way to narrow a type
with a bound, which is why the tutorial writes them out here. See
[exporting by name](programming-model.md#exporting-by-name).

## Step 2: Implement the owner side

The web edge owns the connect point, which means it answers for the auction. There is one
lot under the hammer however many people are watching, and one file that holds it. `synqt
new` already wrote `web/edge/Edge.qml`, the edge itself. Open it and give it the auction.

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
            Caller.emitBidRejected("Your bid must beat " + auction.highBid + ".")
            return
        }
        auction.highBid = amount
        auction.highBidder = bidder
    }
}
```

The three properties are the ones the `export:` block declared, so setting them here is
what publishes them. Every browser watching sees the new value without another line.

`Caller` is whoever made this request. `Caller.emitBidRejected(...)` sends the
`bidRejected` signal back to that one caller rather than to everyone.

The edge is shared, so there is one of these holding one lot. Each caller still arrives
with their own `Caller`, which is what lets the rejection go back to the one browser that
bid too low.

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
                    Server.placeBid(nameField.text, parseInt(amountField.text))
                    amountField.clear()
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

`Server` is how the browser reaches the edge's connect point. `Server.itemName`,
`Server.highBid` and `Server.highBidder` are the live copies of what the edge owns.

## Step 4: Run it

Save everything and look at the browser. You should see the lasagna and a current
bid of 0. Place a bid of 50. The current bid jumps to 50 with your name.

Open the same URL in a second browser tab. Bid 75 in tab two, and watch tab one
update to 75 instantly, with no refresh and no code from you to make that happen.

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

With the check in place, the bid of 10 is rejected and you see the message,
because the edge refuses any bid that does not beat the current high bid.

Delete the check, save, and bid 10 against a standing 50. It wins. The high bid
drops to 10 for everyone.

The rule lives on the owner (the edge), and only there. The browser
never enforced it. If the only check were in the client, anyone could remove it
(it is their browser) and send any bid they liked. This is why in SynQt the owner
of a connect point is the single authority, and every rule that matters lives in
the owner's slot. Put the check back before continuing.

</details>

> [!IMPORTANT]
> Carry this with you for the rest of the tutorial. A consumer asks, and the owner
> decides. Anything you must be able to trust is enforced by the owner, never by
> the consumer. Checks in the UI are only there to be friendly.

## What you learned

- A contract declares the shape of what crosses between two entities.
- A connect point is an owned, named live object, and consumers see a live copy.
- Properties flow owner to consumer, and slots flow consumer to owner.
- The owner is the only authority. Rules live in the owner's slots.
