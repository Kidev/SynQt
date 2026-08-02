<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Routes and URLs

A SynQt client is one WebAssembly bundle, but it is not one page. Every view in it has
a real URL: a visitor can bookmark it, share it, refresh on it, and use Back and Forward
on it, and the address bar says where they are. This page is how that works, end to end,
from the table you write in `synqt.yaml` to the first frame a cold deep link paints.

Three pages hold the exact wording of what is summarized here, and each section below
links to the one that owns its detail:
[`router` and `routes`](project-layout-and-config.md#router-and-routes-client-navigation)
for the configuration, [`Router`](runtime-api.md#client-router) for the QML surface, and
[deep links and the login resume](security.md#deep-links-and-the-login-resume) for what
the edge does with a path it has never heard of.

## The route table

Navigation is configuration, not code. `routes` maps a path to the page shown there, and
`router` says where a refused or unmatched path lands, what prefix the app is served
under, and what a delivered page may import:

```yaml
router:
  fallback: /               # where a refused or unmatched path lands
  base: /                   # the path prefix the app is served under
  palette: [QtQuick, QtQuick.Layouts]

routes:
  - path: /
    view: Home.qml          # compiled into the client bundle

  - path: /c/:campaign
    remote: Campaign.qml    # delivered by the edge, from web/pages/Campaign.qml
    seed: web/campaign-seed.qml

  - path: /admin
    view: Admin.qml
    scope: admin            # below this scope, the router redirects to the fallback
```

A route names its page one of two ways, and the key is the whole of the difference.
`view:` is a QML file compiled into the bundle, downloaded once with everything else.
`remote:` is a QML file the web edge keeps and delivers at navigation time, over the same
authenticated `wss` link, so it never enters the bundle and changes without a client
rebuild; [remote pages](remote-pages.md) is the reference for that half. The two are
mutually exclusive on one route, and everything else on this page is true of both.

Nothing in your QML branches on which kind a route is. One `Loader` renders whatever the
router resolved:

```qml
Loader {
    anchors.fill: parent
    sourceComponent: Router.pageComponent
}
```

## What a URL is made of

An address in a running app has three parts, and the router hands each of them to QML
separately:

| In the address bar | In QML | Comes from |
|--------------------|--------|------------|
| `/shop` | (nothing) | `router.base`, the prefix the app is deployed under. It is stripped before matching and put back when the address bar is written, so the rest of your app never mentions it. |
| `/c/summer-sale` | `Router.path`, `Router.params` | the route table. `/c/:campaign` matched, so `Router.path` is `/c/summer-sale` and `Router.params.campaign` is `summer-sale`. |
| `?page=2&q=hat` | `Router.query` | the query string, split off before matching. `Router.query` is `{ page: "2", q: "hat" }`. |

Captured parameters and query values arrive percent-decoded: `/c/summer%20sale` gives
`Router.params.campaign === "summer sale"`. The three change together, so a binding on any
of them sees a consistent set.

Deployment under a prefix is the one part worth stating twice, because it is where an app
is most often written wrong. With `base: /shop`, a route is still declared as
`/c/:campaign`, `Router.go("/c/summer-sale")` is still the call to make, and
`Router.path` still reads `/c/summer-sale`. Only the address bar carries `/shop`. There is
no second set of paths to keep in step.

## How a path is matched

A route path is a sequence of segments, each either a literal or a `:name` parameter that
captures whatever sits in that position. Two rules decide everything else:

- More literal segments win, whatever the declaration order. `/c/summary` beats
  `/c/:campaign` even when `/c/:campaign` is written first. Precedence is a property of
  the table, not of the order it happens to be written in, so moving a route in
  `synqt.yaml` never silently changes which page a URL opens.
- An empty segment is not a segment. `/c` and `/c/` are one route, and `synqt check`
  refuses a table that declares both rather than leaving one of them unreachable.

`synqt check` also refuses a path that is not absolute, a parameter name that is not an
identifier, one path that repeats a parameter name, a `fallback` that is not itself a
declared route, and a route that claims a path the edge answers itself (its `sync_route`,
and the login routes when the project has an `identity` section). The full list, with the
message each one prints, is in
[validation](project-layout-and-config.md#validation).

## The address bar is the router

There is one navigation mode, `history`: the router drives the browser's History API, so
every route is a real URL rather than a fragment after a `#`.

`Router.go(path)` navigates and adds a history entry. `Router.replace(path)` navigates
without one, so Back skips the page being left, which is what you want after a redirect or
a wizard step. `Router.back()` and `Router.forward()` are the Back and Forward buttons,
and the buttons themselves work because they are the same history.

Two paths through one parameterized route (`/c/spring`, then `/c/summer`) resolve to the
same component, and the router hands back the same instance rather than rebuilding it. The
`Loader` keeps its item alive, and only `path`, `params`, and `query` change. A view that
has to react to that binds `Router.params` rather than doing work in
`Component.onCompleted`, which will not run a second time.

`Router.pageStatus` says why the page on screen is the one showing: `Ready`, `Loading`
(only reachable for a remote page, while the edge is being asked for it), `Forbidden`,
`NotFound`, or `Error`. The [table in the runtime API](runtime-api.md#client-router)
spells out what each one leaves `path` set to.

## A deep link is a cold start

A visitor who bookmarked `/c/summer-sale`, or who pressed refresh while on it, sends the
edge a path that no route of the edge's own answers. The edge serves the application shell
there and the client resolves the path itself, before its link to the edge is even open.

The edge is deliberate about which paths get the shell, because that response is the one
HTML document in the system:

- It is registered as a route, not as a missing-handler hook, so it carries the same
  CSP, COOP, COEP, session cookie, and cache terms as the root document. Served through
  Qt's missing-handler path it would go out with none of them.
- Only `GET` and `HEAD` get it. A `POST` to an unknown URL is a bug or a probe, and
  answering it with HTML would hide that.
- A path whose final segment contains a `.` gets a 404 instead, so a missing asset fails
  as a missing asset rather than as a confusing module load error.

The reasoning behind each of those is in
[deep links and the login resume](security.md#deep-links-and-the-login-resume).

At the moment a deep link resolves, the session holds only the default scope, because the
link to the edge has not opened yet. A scope-gated deep link therefore resolves
`Forbidden` at boot, and is resumed the instant the real scope arrives. That is not a bug
to work around; it is the same guard behaving the same way it does mid-session.

## Guards, refusals, and the login resume

A route's `scope:` is a navigation rule. When a session lacks it, the router goes to
`router.fallback` instead and reports `Forbidden`, and it re-resolves the current route on
every scope change, in both directions: gaining scope promotes a route that was refused,
losing it evicts a visitor from a page they may no longer see and corrects the address bar
with them. Neither is a navigation, so neither adds a history entry.

A refused path is remembered, so signing in lands the visitor where they were going rather
than on the home page with no explanation. Only the path is kept, never the query string,
which may carry a token; the value lives in `sessionStorage`, per tab, and is never sent
to the server. Because anyone can put a link in front of a visitor, the stored path is
validated before anything acts on it, against the rules in
[deep links and the login resume](security.md#deep-links-and-the-login-resume).

> [!IMPORTANT]
> A route guard steers navigation. It is not a secrecy mechanism. The client is one
> compiled bundle, so every compiled-in view's QML reaches every visitor whatever the
> guards say, and the data behind a privileged view stays private only because the
> connect point it reads is scope gated and the owner refuses an under-scoped session.
> A `scope:` on a `remote:` route does keep that page's markup off an under-scoped
> machine, since the edge checks before it delivers a byte, but it still protects the
> markup, not the data. See
> [route guards](programming-model.md#route-guards-which-client-views-are-reachable).

## When there is no address bar

A [native desktop build](desktop.md#navigating-without-an-address-bar) of the same client
runs the same `Router` against the same table, with an in-memory stack in place of the
browser history. There is no deep link to resolve at startup, so it opens on `/`, and
`router.base` is a browser concern that it ignores. `Router.go`, `back()`, `forward()`,
the guards, and the login resume are all unchanged; the resume is held in memory across
the loopback redirect instead of in `sessionStorage`.

## Where to go next

- [Remote pages](remote-pages.md): the `remote:` half of the table, the palette, and the
  page seed that paints a delivered page's first frame.
- [Build it](tutorial-remote-pages-build.md) and
  [Links that work](tutorial-remote-pages-urls.md): the
  [light storefront](tutorial-remote-pages.md) tutorial, where these are hands-on rather
  than described.
- [`Router`](runtime-api.md#client-router): every member, with what each one holds after a
  redirect.
- [`router` and `routes`](project-layout-and-config.md#router-and-routes-client-navigation):
  every configuration key, and what `synqt check` refuses.
