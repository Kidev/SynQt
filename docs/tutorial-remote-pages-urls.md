<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Links that work

The storefront from [part one](tutorial-remote-pages.md) has four routes, and two of them
are delivered by the edge. This page is about the other half of a route: its URL. A campaign
page nobody can link to is a campaign page nobody visits, so the address bar has to be real,
and everything a visitor does to it, bookmark it, refresh it, edit it, press Back, has to
land where they expect.

Three checks you run yourself. Start the app with `synqt dev` and keep a browser tab on it.
[Routes and URLs](routing.md) is the reference behind all three.

## Check 1: the address bar is the page

Click "See today's offers" on the home page. The address bar reads `/c/summer-sale`, and the
headline reads "Summer Sale".

Now copy that URL, open a new tab, and paste it in.

The page opens straight on the campaign. That is worth pulling apart, because two different
pieces of the system cooperated to make it uneventful:

- **The edge answered a path it does not know.** `/c/summer-sale` is a client route. The
  edge has no handler for it, so it serves the application shell there, with the same CSP,
  the same session cookie, and the same cache terms as the root document.
- **The client resolved the URL before it had a connection.** The router reads
  `window.location` at boot and matches it against the compiled route table, so the first
  frame it paints is the campaign, not the home page followed by a jump.

Then the page filled in, in two stages you can see if you watch closely: the headline first,
from the [seed](remote-pages.md#the-page-seed-painting-the-first-frame) the edge built for
this slug, and the offers a moment later, once `Server.catalog` arrived over the `wss` link.
A remote page and a deep link work together here: neither the page's markup nor its data was
in the bundle, and the visitor still landed on a painted page.

Type a slug of your own into the address bar, `/c/black-friday`, and press Enter. Same
`Campaign.qml`, new headline. One page, one route, every campaign.

## Check 2: parameters, query, Back and Forward

One campaign is not enough to watch a parameter change, so give the home page a second one.
In `client/Home.qml`, next to the button you already have:

```qml
Button {
    text: "Black Friday"
    onClicked: Router.go("/c/black-friday")
}
```

That is a client change, so `synqt dev` rebuilds and reloads the client. Add one line to
`web/pages/Campaign.qml` too, inside its `ColumnLayout` under the headline, so the page says
which slug it is showing:

```qml
Text {
    text: "slug: " + (Router.params.campaign ?? "")
    Layout.fillWidth: true
}
```

That one is edge code, so nothing rebuilds: the edge re-reads the page and the tab shows the
new version the next time it renders that route.

Now click between the two campaigns.

Both paths match the same route, `/c/:campaign`, so the router resolves them to the same
component and hands back the same instance rather than rebuilding it. What changes is
`Router.params.campaign`, and with it the seed the page paints from. The page body is not
fetched again either: the client already holds `Campaign.qml` under its content hash, so the
edge answers `notModified` and only the small per-request seed crosses the wire.

That is why the line you added binds `Router.params` instead of reading the slug once in
`Component.onCompleted`, which will not run a second time.

Press Back, then Forward. Both work, and neither reloads the client, because the router
drives the browser's own History API rather than keeping a private stack. `Router.go()` adds
an entry, `Router.replace()` rewrites the current one instead, and `Router.back()` is the
same operation as the button.

Finally, put a query string on the URL: `/c/summer-sale?from=email`. It never takes part in
matching, which is why it did not stop the route from resolving; it arrives whole as
`Router.query`, here `{ from: "email" }`. Path, parameters, and query change together, so a
binding on any one of them sees a consistent set.

## Check 3: a URL the visitor may not have

The stall declares `/members` as `remote: Members.qml` with `scope: user`. You are anonymous
by default (`scopes.default: anonymous` in `synqt.yaml`), so type `/members` into the address
bar and press Enter.

You land on `/`, the `router.fallback`. Two separate things refused you, and it is worth
being clear about which did what:

- **The router** matched `/members`, saw a `scope:` the session lacks, went to the fallback
  instead, and reported `Forbidden`. That is navigation.
- **The edge** never delivered `Members.qml`. It checks a remote route's scope before it
  sends a byte, so an under-scoped fetch comes back with no markup, no content hash, and no
  seed. That is confidentiality.

The path you were refused is remembered, in `sessionStorage`, per tab, and without its query
string. Give the session the `user` scope, and the router replays it: you end up on
`/members`, not on the home page wondering what happened. In the auction that scope comes
from [signing in with a real provider](tutorial-sign-in.md); under `synqt dev` the
[stub identity provider](build-system-and-cli.md#the-development-environment-synqt-dev)
can mint one at any scope for testing, and it is gated so it can never ship.

> [!IMPORTANT]
> The router's half of that is a redirect rule, not a secret. Every compiled-in view's QML
> is in the bundle every visitor downloads, guards or no guards. What keeps privileged data
> private is the owner-side scope check on the connect point that carries it. A `scope:` on
> a `remote:` route does more than steer, since the markup genuinely stays on the edge, but
> it still protects the page, not the data the page reads.

## Try it, then think

> [!QUESTION]
> Ask for `/c/summer.sale`, with a dot instead of a hyphen. It is a perfectly good path, and
> `/c/:campaign` matches it. What comes back, and why is that the right answer?

<details class="solution" markdown>
<summary>Solution</summary>

A 404, not the app.

The edge serves the application shell for a path it does not answer itself, which is what
made Check 1 work. But a path whose **final segment contains a dot** is treated as a request
for an asset, and a missing asset has to fail as one. If the edge sent HTML with a 200 for
`/bundle/client.js` after a bad deploy, the browser would report a module load error deep
inside a script it could not parse, and the actual fact, that the file is not there, would
be nowhere in the message. One rule keeps that honest, and the cost is that a slug cannot
contain a dot.

The same boundary decides a few other things: only `GET` and `HEAD` get the shell, so a
`POST` to an unknown URL fails visibly instead of being answered with a page. All of it is
in [deep links and the login resume](security.md#deep-links-and-the-login-resume).

</details>

## What you learned

- Every route is a real URL. A visitor can bookmark it, share it, refresh on it, and edit
  it, because the edge serves the application shell for any path it does not answer and the
  client resolves that path at boot, before its link to the edge is open.
- A parameterized route is one page: the same component instance survives a parameter
  change, and `Router.params` is what a view binds to notice.
- The query string never takes part in matching. It arrives whole as `Router.query`, and it
  is dropped when a navigation ends anywhere other than the route that was asked for.
- A `scope:` on a route redirects to the fallback and remembers the refused path, so a
  sign-in lands the visitor where they were going. On a `remote:` route the edge also
  refuses to deliver the markup at all.
- A deep link is a cold start: the session holds only the default scope at that moment, so a
  scope-gated deep link resolves `Forbidden` and is resumed as soon as the real scope
  arrives.

## Where to go next

- [Routes and URLs](routing.md): the whole feature in one place, including deployment under
  a path prefix with `router.base` and what changes on a native desktop build.
- [The remote-pages reference](remote-pages.md): the palette, the seed hook, and when a page
  is better off compiled in.
- [`Router`](runtime-api.md#client-router): every member, and what each one holds after a
  redirect.
