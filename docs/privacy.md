<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Privacy and the GDPR

A European app owes its visitors four things a framework can help with: a reachable privacy
policy and legal notice, consent before any cookie that is not strictly necessary, a way to
ask for erasure, and a retention period that is a decision rather than an accident. SynQt
ships one configuration block and three QML types for them.

None of this makes an app compliant. What the app collects, why, and on what legal basis are
questions about the app, and nothing here answers them. What it covers is the plumbing. The
components exist so that the work left over is the part that needs a decision.

## The block

Everything the components read lives in one place in `synqt.yaml`:

```yaml
privacy:
  policy: /privacy                 # a route in this app, or an absolute URL
  legal_notice: /legal             # the imprint most member states require
  contact: privacy@example.com     # the controller contact, Articles 13 and 14
  retention_days: 365              # how long this project keeps personal data
  cookies: []                      # non-essential cookie categories; empty is the default
  erasure: true                    # offer a signed-in visitor an Article 17 request
```

Every key is optional and every value is public. This is what a visitor is entitled to be
told, so all of it is safe in a client that is served to anyone.

## What you get by saying nothing

**Retention defaults to 730 days.** Article 5(1)(e) says personal data is kept no longer than
necessary, and it says so whether or not anyone configured anything, so a project that never
thought about retention should not be storing forever by accident. Two years is the outer
edge of what a supervisory authority routinely accepts for ordinary account records.

Naming a period keeps that period, in both directions. A project that writes 30 keeps 30. The
default fills a gap and never overrides a decision, least of all in the direction that keeps
more data. A project that writes 3650 keeps 3650, because what is lawful depends on what is
stored and why, and several national records statutes require years. `Privacy.retentionDays`
is the value, so a policy page can state the period without a second copy of the number.

**There is no cookie banner.** SynQt sets one cookie, the session credential, and Article 5(3)
of the ePrivacy Directive exempts storage that is strictly necessary to provide the service
the visitor asked for. A session the app does not work without is that. So `privacy.cookies`
starts empty, `CookieConsent` renders nothing while it is, and the banner appears when
somebody adds the category that made it necessary.

Showing a banner anyway is not the cautious choice it looks like. It asks permission for
something that needs none, and it teaches visitors that the way past a consent dialog is to
click whatever makes it go away.

**Erasure is off.** `DataErasureRequest` sends its request no further than the app, because
the app is what knows where its data is. Turning the key on is a project saying somebody has
connected that signal to something that acts.

## The three types

They arrive with `import SynQt` and read the block through the `Privacy` accessor.

### `LegalFooter`

A row of links, for the pages Articles 13 and 14 want reachable from anywhere in the app.

```qml
LegalFooter {
    onNavigate: url => Router.go(url)
}
```

A link whose URL the project did not declare is left out rather than shown broken. It also
carries the way back to the cookie banner once a visitor has answered, because withdrawing
consent has to be as easy as giving it was (Article 7(3)) and a footer is somewhere they can
find it.

### `CookieConsent`

The banner, invisible until `privacy.cookies` names a category and gone once this visitor has
answered.

```qml
CookieConsent {
    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
    onAnswered: granted => analytics.enabled = granted.includes("analytics")
}
```

Refusing is one tap, in the same place and the same weight as accepting, because consent
under Article 4(11) is freely given, and a banner where refusing is harder than accepting does not
collect any. Every box starts unticked, because a box ticked in advance is not an unambiguous
indication of anything.

The answer is filtered twice against what the project declared. A page that calls
`Privacy.accept()` with a category nobody wrote down does not get it, and an answer stored
before a category was removed does not survive into the configuration that no longer has it.

Read a category with `Privacy.hasConsent("analytics")`, which is false until this visitor says
otherwise. Silence is not consent, and a page that runs a script while the banner is still up
has not been permitted to.

### `DataErasureRequest`

The Article 17 request, as a button and a confirmation.

```qml
DataErasureRequest {
    id: erasure

    onConfirmed: Server.eraseMe().then(() => erasure.reportAccepted(),
                                               error => erasure.reportFailed(error))
}
```

Visible only to a signed-in visitor, and only where `privacy.erasure` is on. An anonymous
visitor's request names no data to erase, and `synqt check` refuses a project that offers the
button with no `identity:` block at all, because nobody there is ever signed in.

The confirmation carries the consequence, which is the same everywhere: erasure ends the
account and cannot be undone. Where a retention period is configured, it says so, because the
records a law obliges the project to keep are the part a visitor is most likely to be
surprised by.

## Where the session cookie fits

The session credential is httpOnly, Secure and SameSite, it holds no personal data (it is a
random identifier the edge resolves), and it is what makes the app work at all. That is the
Article 5(3) exemption, and it is why a SynQt app with no other cookie has no banner to show.
[Security](security.md) covers what the cookie is and what the edge does with it.
[Authentication](authentication.md) covers what identity data the edge holds and where.
