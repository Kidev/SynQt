<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Links that work

The storefront from [part one](tutorial-remote-pages-build.md) has four routes, and the edge
delivers two of them. This page covers the other half of a route: its URL. A campaign page
needs a link people can share, so the address bar must be real. Whatever a visitor does
with it (bookmark, refresh, edit, press Back) must land where they expect.

Run the three checks below. Start the app with `synqt dev` and keep a browser tab on it.
[Routes and URLs](routing.md) is the reference for all three.

## Check 1: the address bar is the page

Click "See today's offers" on the home page. The address bar reads `/c/summer-sale`, and the
headline reads "Summer Sale".

Now copy that URL, open a new tab, and paste it in.

The page opens directly on the campaign. Two parts of the system make that work:

- **The edge answers a path it does not know.** `/c/summer-sale` is a client route, and
  the edge has no handler for it, so it serves the application shell there, with the same
  CSP, session cookie and cache headers as the root document.
- **The client resolves the URL before it connects.** The router reads `window.location` at
  startup and matches it against the compiled route table, so its first frame is the
  campaign, not the home page followed by a jump.

The page then fills in, in two visible stages: first the headline, from the
[seed](remote-pages.md#the-page-seed-painting-the-first-frame) the edge built for this slug,
then the offers, once `Server.offers` arrives over the `wss` link. The bundle held neither
the page's markup nor its data, and the visitor still landed on a painted page.

Type your own slug in the address bar, such as `/c/black-friday`, and press Enter. The same
`Campaign.qml` shows a new headline, because one route serves every campaign.

## Check 2: parameters, query, Back and Forward

To watch a parameter change you need a second campaign. In `client/app/Home.qml`, next to
the existing button, add:

```qml
Button {
    text: "Black Friday"
    onClicked: Router.go("/c/black-friday")
}
```

That is a client change, so `synqt dev` rebuilds and reloads the client. Also add one line
to `web/edge/pages/Campaign.qml`, inside its `ColumnLayout` under the headline, so the page
shows which slug it displays:

```qml
Text {
    text: "slug: " + (Router.params.campaign ?? "")
    Layout.fillWidth: true
}
```

That is edge code, so nothing rebuilds. The edge re-reads the page and tells the open tab,
which fetches it again and renders it in place.

Now click between the two campaigns.

Both paths match the route `/c/:campaign`, so the router resolves them to the same
component and keeps the same instance instead of rebuilding it. `Router.params.campaign`
changes, and with it the seed the page paints from. The client keeps the page body: it
already holds `Campaign.qml` under its content hash, so the edge answers `notModified` and
only the small per-request seed crosses the wire.

That is why the line you added binds to `Router.params` instead of reading the slug once in
`Component.onCompleted`, which does not run a second time.

Press Back, then Forward. Both work without reloading the client, because the router uses
the browser's History API instead of a private stack. `Router.go()` adds an entry,
`Router.replace()` rewrites the current one, and `Router.back()` does what the Back button
does.

Finally, add a query string to the URL: `/c/summer-sale?from=email`. A query string never
takes part in matching, so the route still resolves. It arrives whole as `Router.query`,
here `{ from: "email" }`. Path, parameters and query change together, so a binding on any
of them sees a consistent set.

## Check 3: a URL the visitor may not have

The stall declares `/members` as `remote: Members.qml` with `scope: user`. You are anonymous
by default (`scopes.default: anonymous` in `synqt.yaml`), so type `/members` in the address
bar and press Enter.

You land on `/`, the `router.fallback`. Two separate things refused you:

- **Navigation.** The router matched `/members`, saw a `scope:` the session lacks, went to
  the fallback, and reported `Forbidden`.
- **Confidentiality.** The edge never delivered `Members.qml`. It checks a remote route's
  scope before sending a byte, so a fetch below that scope comes back with no markup, no
  content hash and no seed.

The client remembers the refused path in `sessionStorage`, per tab, without its query
string. Once the session gets the `user` scope, the router replays it, and you land on
`/members` instead of the home page. In the auction, that scope comes from
[signing in with a real provider](tutorial-sign-in.md). Until you register one, the
[development sign-in](authentication.md#the-development-sign-in) lets you sign in as any
configured person your mapping hook raises; three separate gates keep it out of any
build you ship.

> [!IMPORTANT]
> The router's refusal is a redirect, not a secret. Every compiled-in view's QML is in the
> bundle every visitor downloads, guards or not. The owner side scope check on the connect
> point that carries privileged data is what keeps it private. A `scope:` on a `remote:`
> route does more than steer, since the markup really stays on the edge, but it still
> protects the page, not the data the page reads.

## Try it, then think

> [!QUESTION]
> Ask for `/c/summer.sale`, with a dot instead of a hyphen. It is a valid path, and
> `/c/:campaign` matches it. What comes back, and why is that right?

<details class="solution" markdown>
<summary>Solution</summary>

A 404, not the app.

The edge serves the application shell for any path it does not answer itself, which is
what made check 1 work. But it treats a path whose last segment contains a dot as an asset
request, and a missing asset must fail as one. If the edge answered a missing
`/bundle/client.js` with HTML and a 200 after a bad deploy, the browser would report a
module load error deep inside a script it could not parse, and nothing in the message
would say the file is missing. The cost of this rule is that a slug cannot contain a dot.

The same boundary settles a few other cases: only `GET` and `HEAD` get the shell, so a
`POST` to an unknown URL fails visibly instead of returning a page. See
[deep links and the login resume](security.md#deep-links-and-the-login-resume).

</details>

## What you learned

- Every route is a real URL. A visitor can bookmark, share, refresh and edit it, because
  the edge serves the application shell for any path it does not answer, and the client
  resolves the path at startup, before its link to the edge opens.
- A route with a parameter is one page. The same component instance survives a parameter
  change, and a view binds to `Router.params` to notice it.
- The query string never takes part in matching. It arrives whole as `Router.query`, and is
  dropped when a navigation ends anywhere other than the requested route.
- A `scope:` on a route redirects to the fallback and remembers the refused path, so signing
  in takes the visitor where they were going. On a `remote:` route, the edge also refuses
  to deliver the markup.
- A deep link is a cold start. At that moment the session holds only the default scope, so
  a scope-gated deep link resolves `Forbidden`, then resumes as soon as the real scope
  arrives.

## Where to go next

- [Routes and URLs](routing.md): the whole feature, including deploying under a path
  prefix with `router.base`, and what changes on a native desktop build.
- [The remote pages reference](remote-pages.md): the palette, the seed hook, and when a page
  is better compiled in.
- [`Router`](runtime-api.md#client-router): every member, and what each holds after a
  redirect.
