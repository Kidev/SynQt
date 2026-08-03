<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Cutting a release

The auction is running on two hosts. Now for the part nobody writes down and everybody
needs at four in the afternoon on a Friday: what a release is, how a browser that already
has your client gets the new one, what signing a desktop app actually costs, what
shipping a client obliges you to publish, and how to put it all back if you were wrong.

## Step 1: A release is a tag and an artifact

Nothing exotic. Tag the commit, let the pipeline you wrote in
[The pipeline that says no](tutorial-ship-pipeline.md) build it, keep the artifact.

```cli
git tag -a v1.0.0 -m "First public auction"
git push origin v1.0.0
```

The rule that makes this a release rather than a build is the one from the pipeline page:
**build once, deploy that**. The artifact from the tag goes to staging, and the same
artifact goes to production. If production gets its own build, then whatever staging
proved was proved about a different set of bytes.

Two habits are worth having from the first release:

- **The version is in the artifact name and in the deployment path.** `/srv/gavel-v1.0.0`
  with `/srv/gavel` a symlink to it. Then "what is running" has an answer you can read
  with `ls -l`.
- **Keep the previous one.** Disk is cheaper than the fifteen minutes it takes to rebuild
  a release you deleted while it was on fire.

## Step 2: Deploy it

With the symlink layout, a deploy is four commands and a rollback is two.

```cli
# on each host
rsync -a gavel-v1.0.0/ /srv/gavel-v1.0.0/
cp -a /srv/gavel/synqt/mesh /srv/gavel/*/.env /srv/gavel-v1.0.0/...   # the material that stays
ln -sfn /srv/gavel-v1.0.0 /srv/gavel
sudo systemctl restart gavel-database   # owners first, per process-manifest.json
sudo systemctl restart gavel-web
```

The certificates and the env files stay with the host, not with the release. They are not
build output and they are not in the artifact, which is exactly the arrangement
[Two authorities](tutorial-ship-certificates.md) set up.

Restart in `start_order`. A consumer retries, so the reverse order is survivable rather
than correct; doing it right means a restart nobody has to watch.

> [!TIP]
> A rollback is `ln -sfn /srv/gavel-v0.9.0 /srv/gavel` and the same two restarts. Practise
> it once, on purpose, on a day when nothing is wrong. A rollback you have never run is a
> plan, not a capability.

## Step 3: How a browser gets the new client

Your visitors already have the old client in their browser. Here is what happens to them,
and it needs nothing from you.

Every build stamps a build id into `build/client/synqt-manifest.json`. With the default
`build.client_cache: service_worker`, a repeat visit is served from CacheStorage with no
network on the critical path, and the worker then fetches the manifest in the background
and compares that id. Identical is the common case and ends there. A real change pulls the
new module and raises an update, which the client surfaces through the
[`App`](runtime-api.md#client-app) accessor, so your QML decides whether to prompt or to
apply it on the next navigation.

The edge sends `Cache-Control: no-cache` on every bundle file, which means revalidate
rather than do not store. That is what keeps the check cheap and what stops a browser
pinning a stale worker forever.

Two consequences worth planning for:

- **A visitor mid-session keeps the client they loaded with.** They are not interrupted,
  and they are also not on your new code until they come back. Roll out edge changes that
  the old client can still talk to, or accept a window where both are live. A contract is
  the boundary that makes this manageable: the same `.syn` generates both ends, so an
  incompatible change is a compile error somewhere rather than a mystery in production.
- **If your deployment does not allow service workers**, set `build.client_cache: http`
  and the edge's `ETag` layer does the job with one conditional request per visit. Slower,
  simpler, no CacheStorage quota.

## Step 4: The desktop client, if you ship one

The auction's client entity can also be built as a native app. Nothing about the
deployment changes: it reaches the same edge over the same `wss://` link, holds no secret
and no mesh certificate, and is authorized by the same user sessions. What changes is that
you are now handing an executable to a stranger's machine.

```cli
synqt build --client desktop --release --profile production
```

That gives you a binary under `build/client-desktop/<platform>/` and a `DEPLOY.txt`
naming the platform step that makes it carry its own Qt. The step is not run by default,
because signing identities, entitlements, notarization and installer format are not a
framework's to choose. Ask for it and say what you mean:

```cli
synqt build --client desktop --release --deploy --sign "Developer ID Application: Acme (AB12CD34)"
synqt build --client desktop --release --deploy --unsigned
```

Neither flag has a default, because the cost of not signing is different everywhere and
only one of the three answers is "it will not run":

| Platform | Unsigned binary | Signing is |
|----------|-----------------|------------|
| macOS | Gatekeeper refuses it anywhere but the machine that built it | **required** to distribute |
| Windows | runs, but SmartScreen warns every downloader about an unrecognised publisher | **strongly advised** |
| Linux | runs normally; there is no binary code signing | **not applicable**, sign the *package* |

Two things SynQt will not do, and says so rather than half doing:

- **It does not notarize.** That needs your Apple credentials and a network round trip.
  `DEPLOY.txt` hands you the `notarytool` command; running it is yours.
- **It does not cross compile a desktop app.** A native build uses the host's Qt kit, so
  the Windows app is built on Windows and the macOS app on macOS. A CI matrix with three
  runners is the normal answer, and it is the same four commands per runner.

Set the bundle identifier once, on the generated preset, since it belongs with signing:

```cli
cmake --preset host -DSYNQT_BUNDLE_ID=com.acme.gavel
```

[Desktop clients](desktop.md#building-for-desktop) has the rest, including exactly what
the Linux portable layout copies and how it is verified.

## Step 5: The obligation you cannot skip

Read the last lines of your release build. Under open source Qt they say something like:

```text
note: this client is built with open source Qt and is conveyed to every visitor,
      so its source must be offered under GPLv3. A commercial Qt license is the
      alternative. See docs/licensing.md.
```

That is not boilerplate. A browser client is **conveyed**: every visitor receives a copy
of the program, which is the trigger the GPL is written around. The self hosted edge is
the dormant case (nobody receives it, so nothing is triggered) right up until you
distribute the binary or ship a desktop client, and then it is not dormant any more.

What discharges it, concretely:

- **Offer the complete corresponding source** of the conveyed work under GPLv3, including
  your own application code that was compiled into it. A public repository is the least
  effort way; a written offer is the other legal way.
- **Include the licence texts and prominent notices.** `synqt build` generates a
  `THIRD-PARTY-LICENSES` per entity, and one per client target, derived from what that
  artifact actually links rather than maintained by hand. Ship it, and surface the notices
  in the client itself.
- **Or buy a commercial Qt licence**, in which case none of the above applies and you may
  keep the client closed.

The full analysis, per module and per entity, is in [licensing](licensing.md), and the
short version is in its [obligations
checklist](licensing.md#obligations-checklist). Decide which of the two paths you are on
before your first public release, not after.

## Step 6: The certificate that expires while you are not looking

Entity certificates are good for 398 days. On the anniversary of your first deploy, they
stop working, and the failure looks like a networking fault: the edge cannot reach the
database, the log says the handshake failed, and nothing about the code changed.

Rotating one entity needs no coordination, because its peers verify against the CA
certificate and that did not change:

```cli
# on the machine that holds ca.key, not on a host
synqt mesh status
synqt mesh rotate database
```

then copy the new `database.crt` and `database.key` to the database host and restart that
entity. The edge notices nothing beyond a reconnect.

Rotating the **authority** is the one to schedule. Every entity trusts exactly one CA
certificate, so there is no overlap to hide behind: a new authority means new leaves
everywhere and a coordinated restart. Put it in the calendar next to the CA's own expiry,
which is twice the leaf lifetime away.

## What you learned

- A deployment adds exactly four things to what `synqt dev` gave you: real certificates,
  real secrets, real TLS to the browser, and something that keeps the processes running.
  Everything else is arrangement.
- The pipeline's job is to refuse. `synqt check --release` asks the production question
  against the profile you will actually deploy, and it asks it before anything compiles.
- A SynQt deployment is a project directory. Every path an entity reads is relative to it,
  which is what lets you look at a host and see the whole system.
- The CA private key is not a deployment input. It never reaches a host that runs an
  entity and never reaches CI, because whoever holds it can be any entity in your system.
- `build/process-manifest.json` is the start plan: owners before consumers, exactly one
  public bind, and the material each entity expects.
- Build once and deploy that artifact. Keep the previous release on disk, and make the
  rollback a symlink you have moved before.
- A client is conveyed to everyone who loads it. Under open source Qt that is a GPLv3
  source obligation, and the tooling tells you so on every release build.

## Where to go next

- [Deploying a SynQt system](deploying.md): the same path as an ordered checklist, without
  the tutorial around it. This is the page to keep open during a deploy.
- [The security checklist](security.md#security-checklist-use-before-every-deploy):
  short, and written to be read at deploy time.
- [Build system and CLI](build-system-and-cli.md): every command, every flag, and the
  toolchain pinning that makes a build reproducible.
- [Desktop clients](desktop.md): the whole native story, including what a deployed tree
  contains and how the framework verifies it carries its own Qt.
- [Licensing](licensing.md): which artifact is under which licence, and what each one
  obliges you to do.
