<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Routes and URLs

A SynQt client is one WebAssembly bundle but many pages. Every view has a real URL that a
visitor can bookmark, share, refresh and navigate with Back and Forward, and the address
bar shows where they are. This page explains how, from the table you write in `synqt.yaml`
to the first frame of a cold deep link.

Three pages hold the full detail, and each section below links to the relevant one:
[`router` and `routes`](project-layout-and-config.md#router-and-routes-client-navigation)
for the configuration, [`Router`](runtime-api.md#client-router) for the QML surface, and
[deep links and the login resume](security.md#deep-links-and-the-login-resume) for what
the edge does with a path it has never heard of.

## The route table

Navigation is configuration. `routes` maps each path to its page, and `router` says where
a refused or unmatched path lands, which prefix the app is served under, and what a
delivered page may import:

```yaml
router:
  fallback: /               # where a refused or unmatched path lands
  base: /                   # the path prefix the app is served under
  palette: [QtQuick, QtQuick.Layouts]

routes:
  - path: /
    view: Home.qml          # compiled into the client bundle

  - path: /c/:campaign
    remote: Campaign.qml    # delivered by the edge, from web/edge/pages/Campaign.qml
    seed: web/edge/campaign-seed.qml

  - path: /admin
    view: Admin.qml
    scope: admin            # below this scope, the router redirects to the fallback
```

A route names its page in one of two ways:

- **`view:`** is a QML file compiled into the bundle and downloaded once with everything
  else.
- **`remote:`** is a QML file the web edge keeps and delivers when the visitor navigates,
  over the same authenticated `wss` link. It never enters the bundle, and it changes
  without a client rebuild. See [remote pages](remote-pages.md).

A route has one or the other, never both. The rest of this page applies to both.

### Where the table lives

A top level `routes:` is the table of the project's client. A project with several clients
(a landing page and the application, or an application and an operator console) gives each
its own table, on the entity:

```yaml
entities:
  - name: app
    type: client
    routes:
      - path: /
        view: Home.qml
```

The top level list is shorthand for a project with exactly one client. If two clients
would both fall back to it, `synqt check` refuses the topology instead of picking one:
whichever entity the generator rendered first would take the table, and the other would
compile with an empty one.

Which client a visitor gets in the first place is a separate question, answered by
[`bundles:`](project-layout-and-config.md) on the web edge. A route guard decides where a
visitor may go inside the bundle they hold; a bundle decides which one they receive.

Your QML never checks what kind a route is. One `Loader` renders whatever the router
resolves:

```qml
Loader {
    anchors.fill: parent
    sourceComponent: Router.pageComponent
}
```

## What a URL is made of

An address in a running app has three parts, and the router gives each to QML
separately:

| In the address bar | In QML | Comes from |
|--------------------|--------|------------|
| `/shop` | (nothing) | `router.base`, the prefix the app is deployed under. It is stripped before matching and put back when the address bar is written, so the rest of your app never mentions it. |
| `/c/summer-sale` | `Router.path`, `Router.params` | the route table. `/c/:campaign` matched, so `Router.path` is `/c/summer-sale` and `Router.params.campaign` is `summer-sale`. |
| `?page=2&q=hat` | `Router.query` | the query string, split off before matching. `Router.query` is `{ page: "2", q: "hat" }`. |

Captured parameters and query values arrive percent-decoded, so `/c/summer%20sale` gives
`Router.params.campaign === "summer sale"`. All three change together, so a binding on any
of them sees a consistent set.

Deploying under a prefix changes nothing in the app. With `base: /shop`, you still declare
the route as `/c/:campaign`, still call `Router.go("/c/summer-sale")`, and `Router.path`
still reads `/c/summer-sale`. Only the address bar shows `/shop`, so there is no second set
of paths to maintain.

## How a path is matched

A route path is a sequence of segments, each either a literal or a `:name` parameter that
captures whatever is in that position. Two rules decide the rest:

- **More literal segments win,** whatever the declaration order. `/c/summary` beats
  `/c/:campaign` even when `/c/:campaign` comes first, so reordering routes in
  `synqt.yaml` never changes which page a URL opens.
- **Empty segments do not count.** `/c` and `/c/` are the same route, and `synqt check`
  refuses a table that declares both, instead of leaving one unreachable.

`synqt check` also refuses a path that is not absolute, a parameter name that is not an
identifier, a path that repeats a parameter name, a `fallback` that is not a declared
route, and a route that claims a path the edge answers itself (its `sync_route`, and the
login routes when the project has an `identity` section).
[Validation](project-layout-and-config.md#validation) lists them all, with each message.

## The address bar is the router

There is one navigation mode, `history`: the router drives the browser's History API, so
every route is a real URL, not a fragment after a `#`.

`Router.go(path)` navigates and adds a history entry. `Router.replace(path)` navigates
without one, so Back skips the page you left, which suits a redirect or a wizard step.
`Router.back()` and `Router.forward()` do what the browser's buttons do, and the buttons
work too, since they share the same history.

Two paths through one route with a parameter (`/c/spring`, then `/c/summer`) resolve to
the same component, and the router keeps the same instance instead of rebuilding it. The
`Loader` keeps its item, and only `path`, `params` and `query` change. A view that must
react binds to `Router.params` instead of working in `Component.onCompleted`, which does
not run a second time.

`Router.pageStatus` says why the current page is showing: `Ready`, `Loading` (only for a
remote page, while the edge is asked for it), `Forbidden`, `NotFound`, `Unsupported` (the
route needs an accelerated scene graph this browser did not give Qt, so a notice shows in
its place) or `Error`. The [table in the runtime API](runtime-api.md#client-router) says
what `path` holds in each case.

## A deep link is a cold start

A visitor who bookmarked `/c/summer-sale`, or refreshed on it, sends the edge a path none
of its own routes answer. The edge serves the application shell there, and the client
resolves the path itself, before its link to the edge even opens.

The shell is the system's only HTML document, so the edge restricts which paths get it:

- **It is a registered route,** not a missing handler hook, so it carries the same CSP,
  COOP, COEP, session cookie and cache headers as the root document. Through Qt's missing
  handler path, it would carry none of them.
- **Only `GET` and `HEAD` get it.** A `POST` to an unknown URL is a bug or a probe, and HTML
  would hide that.
- **A path whose last segment contains a `.` gets a 404,** so a missing asset fails as a
  missing asset, not as a confusing module load error.

[Deep links and the login resume](security.md#deep-links-and-the-login-resume) explains
each rule.

When a deep link resolves, the session holds only the default scope, because the link to
the edge is not open yet. So a scope-gated deep link resolves `Forbidden` at startup and
resumes as soon as the real scope arrives. The guard works the same way mid-session.

## Guards, refusals, and the login resume

A route's `scope:` is a navigation rule. When the session lacks it, the router goes to
`router.fallback` and reports `Forbidden`. It re-resolves the current route on every scope
change, in both directions: gaining a scope opens a route that was refused, and losing one
moves the visitor off a page they may no longer see and corrects the address bar. Both
happen outside navigation, so neither adds a history entry.

The router remembers a refused path, so signing in takes the visitor where they were going,
not to the home page without explanation. It keeps only the path, never the query string,
which may carry a token, in `sessionStorage`, per tab, never sent to the server. Anyone can
show a visitor a link, so the stored path is validated before use, against the rules in
[deep links and the login resume](security.md#deep-links-and-the-login-resume).

> [!IMPORTANT]
> A route guard only steers navigation. The client is one compiled
> bundle, so every compiled-in view's QML reaches every visitor, whatever the guards say.
> A privileged view's data stays private only because the connect point it reads is
> scope gated and the owner refuses sessions below that scope. A `scope:` on a `remote:`
> route does keep that page's markup off the visitor's machine, since the edge checks
> before delivering a byte, but it protects the markup, not the data. See
> [route guards](programming-model.md#route-guards-which-client-views-are-reachable).

## When there is no address bar

A [native desktop build](desktop.md#navigating-without-an-address-bar) of the same client
runs the same `Router` against the same table, with a history stack in memory instead of
the browser's. It always opens on `/`, with no deep link at startup, and ignores
`router.base`, which only matters in a browser. `Router.go`, `back()`, `forward()`, the
guards and the login resume work the same; the resume stays in memory across the loopback
redirect instead of in `sessionStorage`.

## Where to go next

- [Remote pages](remote-pages.md): the `remote:` half of the table, the palette, and the
  page seed that paints a delivered page's first frame.
- [Build it](tutorial-remote-pages-build.md) and
  [Links that work](tutorial-remote-pages-urls.md): the
  [light storefront](tutorial-remote-pages.md) tutorial, where you try all of this
  yourself.
- [`Router`](runtime-api.md#client-router): every member, with what each one holds after a
  redirect.
- [`router` and `routes`](project-layout-and-config.md#router-and-routes-client-navigation):
  every configuration key, and what `synqt check` refuses.
