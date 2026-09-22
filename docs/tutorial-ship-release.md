<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Cutting a release

The auction runs on two hosts. This page covers what a release is, how a browser that has
your old client gets the new one, what signing a desktop app costs, what shipping a client
obliges you to publish, and how to roll back.

## Step 1: A release is a tag and an artifact

Tag the commit, let the pipeline from
[The pipeline that says no](tutorial-ship-pipeline.md) build it, and keep the artifact.

```cli
git tag -a v1.0.0 -m "First public auction"
git push origin v1.0.0
```

The rule from the pipeline page makes this a release: build once and deploy that build.
The tag's artifact goes to staging, and the same artifact goes to production. If
production gets its own build, staging tested different bytes.

Adopt two habits from the first release:

- **Put the version in the artifact name and the deployment path,** such as
  `/srv/gavel-v1.0.0` with `/srv/gavel` a symlink to it. Then `ls -l` tells you what is
  running.
- **Keep the previous release.** Disk is cheaper than fifteen minutes spent rebuilding a
  release you deleted during an incident.

## Step 2: Deploy it

With the symlink layout, a deploy is a copy, the host's own files and a symlink, and a
rollback is the symlink alone. Both end with a restart.

```cli
# on each host: the release, and the certificates that stay with the host
rsync -a gavel-v1.0.0/ /srv/gavel-v1.0.0/
cp -a /srv/gavel/synqt/mesh /srv/gavel-v1.0.0/synqt/
# on the edge host: its env file
cp -a /srv/gavel/web/edge/.env /srv/gavel-v1.0.0/web/edge/
# on the books host: its env file
cp -a /srv/gavel/db/relational/books/.env /srv/gavel-v1.0.0/db/relational/books/
# on each host
ln -sfn /srv/gavel-v1.0.0 /srv/gavel
sudo systemctl restart gavel-books      # on the books host first, per process-manifest.json
sudo systemctl restart gavel-edge       # then on the edge host
```

The certificates and env files belong to the host, not to the release: they stay outside
the build output and the artifact, as [Two authorities](tutorial-ship-certificates.md) set
up. The database stays outside every release too: the production profile keeps it in
`/srv/gavel-data/books/`, so a deploy and a rollback both find it where it was.

Restart in `start_order`. A consumer retries, so the reverse order survives, but the right
order gives a restart nobody has to watch.

> [!TIP]
> A rollback is `ln -sfn /srv/gavel-v0.9.0 /srv/gavel` and the same two restarts. Practice
> it once, on a day when nothing is wrong.

## Step 3: How a browser gets the new client

Your visitors already have the old client in their browser. Updating them needs nothing
from you.

Every build stamps a build id into `build/client/synqt-manifest.json`. With the default
`build.client_cache: service_worker`, a repeat visit loads from CacheStorage without
waiting on the network, then the worker fetches the manifest in the background and
compares the id. Usually it matches, and that is all. On a real change, the worker fetches
the new module and raises an update, which the client exposes through the
[`App`](runtime-api.md#client-app) accessor; your QML decides whether to prompt or apply it
on the next navigation.

The edge sends `Cache-Control: no-cache` on every bundle file, which means "revalidate",
not "do not store". That keeps the check cheap and stops a browser from holding a stale
worker forever.

Plan for two consequences:

- **A visitor mid-session keeps the client they loaded.** Nothing interrupts them, but
  they run your new code only when they return. Roll out edge changes the old client can
  still talk to, or accept a period when both are live. The contract makes this
  manageable: one declaration generates both ends, so an incompatible change fails to
  compile instead of failing mysteriously in production.
- **If your deployment forbids service workers,** set `build.client_cache: http`. The edge's
  `ETag` layer then does the job with one conditional request per visit: slower, simpler,
  and no CacheStorage quota.

## Step 4: The desktop client, if you ship one

The auction's client entity can also build as a native app. The deployment stays the
same: the app reaches the same edge over the same `wss://` link, holds no secret and no
mesh certificate, and uses the same user sessions. What changes is that you now hand an
executable to a stranger's machine.

```cli
synqt build --client desktop --release --profile production
```

This produces a binary under `build/client-desktop/<platform>/`, and a `DEPLOY.txt` naming
the platform step that bundles Qt with it. The step does not run by default, because
signing identities, entitlements, notarization and installer format are not a
framework's choice. Ask for it explicitly:

```cli
synqt build --client desktop --release --deploy --sign "Developer ID Application: Acme (AB12CD34)"
synqt build --client desktop --release --deploy --unsigned
```

You must pick one, because the cost of not signing differs per platform, and only one
platform refuses to run the app:

| Platform | Unsigned binary | Signing is |
|----------|-----------------|------------|
| macOS | Gatekeeper refuses it anywhere but the machine that built it | **required** to distribute |
| Windows | runs, but SmartScreen warns every downloader about an unrecognised publisher | **strongly advised** |
| Linux | runs normally, since there is no binary code signing | **not applicable**, sign the package |

SynQt leaves two things to you, and says so:

- **Notarization.** That needs your Apple credentials and a network round trip.
  `DEPLOY.txt` gives you the `notarytool` command to run yourself.
- **Cross compiling a desktop app.** A native build uses the host's Qt kit, so
  you build the Windows app on Windows and the macOS app on macOS. A CI matrix with three
  runners, each running the same four commands, is the usual answer.

Set the bundle identifier once, on the generated preset, since it belongs with signing:

```cli
cmake --preset host -DSYNQT_BUNDLE_ID=com.acme.gavel
```

[Desktop clients](desktop.md#building-for-desktop) covers the rest, including what the
Linux portable layout copies and how it is verified.

## Step 5: The obligation you cannot skip

Read the last lines of your release build. Under open source Qt they say:

```text
Note: built with open-source Qt, your client is GPLv3 and is served to every visitor,
so you must publish its source. Use a commercial Qt license to keep it closed.
See https://synqt.org/licensing/.
Note: distributing the edge binary triggers GPLv3 (Qt HTTP Server / Network
Authorization). See https://synqt.org/licensing/.
```

This matters. A browser client is conveyed: every visitor receives a copy of the program,
which is what triggers the GPL. A self hosted edge triggers nothing, since nobody receives
it, until you distribute the binary or ship a desktop client.

To meet the obligation:

- **Offer the complete corresponding source** of the conveyed work under GPLv3, including
  your own application code compiled into it. A public repository is the easiest way; a
  written offer is the other legal way.
- **Include the license texts and prominent notices.** `synqt build` generates a
  `THIRD-PARTY-LICENSES` per entity and per client target, derived from what each artifact
  links. Ship it, and show the notices in the client itself.
- **Or buy a commercial Qt license.** Then none of this applies, and you may keep the
  client closed.

[Licensing](licensing.md) has the full analysis per module and per entity, and its
[obligations checklist](licensing.md#obligations-checklist) has the short version. Choose
your path before your first public release.

## Step 6: The certificate that expires while you are not looking

Entity certificates last 398 days. When they expire, the failure looks like a network
fault: the edge cannot reach the database, the log says the handshake failed, and the code
has not changed.

Rotating one entity needs no coordination, because its peers verify against the unchanged
CA certificate:

```cli
# on the machine that holds ca.key, not on a host
synqt mesh status
synqt mesh rotate books
```

Then copy the new `books.crt` and `books.key` to the database host and restart that
entity. The edge sees only a reconnect.

Schedule an authority rotation. Every entity trusts exactly one CA certificate, so the
rotation has no overlap period: a new authority means new leaves everywhere and a coordinated restart.
Put it in the calendar before the CA expires, which is twice the leaf lifetime away.

## What you learned

- A deployment adds four things to what `synqt dev` provides: real certificates, real
  secrets, real TLS to the browser, and something that keeps the processes running.
- The pipeline's job is to refuse. `synqt check --release` checks the profile you will
  deploy against the production rules, before anything compiles.
- A SynQt deployment is a project directory. Every path an entity reads is relative to it,
  so you can look at a host and see the whole system.
- The CA private key stays out of every deployment. It never reaches a host that runs an
  entity, or CI, because whoever holds it can impersonate any entity.
- `build/process-manifest.json` is the start plan: owners before consumers, exactly one
  public bind, and the files each entity expects.
- Build once and deploy that artifact. Keep the previous release on disk, and practice
  the rollback (moving a symlink).
- A client is conveyed to everyone who loads it. Under open source Qt that is a GPLv3
  source obligation, and every release build reminds you.

## Where to go next

- [Deploying a SynQt system](deploying.md) is the same path as an ordered checklist. Keep
  it open during a deploy.
- [The security checklist](security.md#security-checklist) is short and meant for deploy
  time.
- [Build system and CLI](build-system-and-cli.md) covers every command and flag, and the
  toolchain pinning that makes a build reproducible.
- [Desktop clients](desktop.md) covers native builds, including what a deployed tree
  contains and how the framework checks that it carries its own Qt.
- [Licensing](licensing.md) says which license covers each artifact, and what each obliges
  you to do.
