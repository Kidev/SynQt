<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Where the binaries go

A SynQt deployment is a whole project directory.

Every entity finds its runtime files relative to the directory it starts from, exactly as
`synqt.yaml` spells them: its topology under `build/<entity>/`, its certificate under
`synqt/mesh/`, its secrets in its own `.env`, its schema and data in its own folder, and,
for the edge, the client bundle under `build/client/`. Copy `build/edge/edge` somewhere
alone and it starts, looks for all of that, and finds none of it.

## Step 1: The shape on each host

Take the artifact your pipeline produced and keep only what each host runs.

The edge host:

```text
/srv/gavel/
  synqt.yaml
  synqt.production.yaml
  certs/edge/fullchain.pem
  certs/edge/privkey.pem
  synqt/mesh/
    ca.crt
    edge.crt
    edge.key
  build/
    edge/             # the edge binary and its topology.json
    client/           # the bundle it serves
  web/edge/
    .env              # the OAuth client secret
```

The books host:

```text
/srv/gavel/
  synqt.yaml
  synqt.production.yaml
  synqt/mesh/
    ca.crt
    books.crt
    books.key
  build/
    books/            # the binary and its topology.json
  db/relational/books/
    .env
    schema.sql
    data/             # the SQLite file lives here
```

Three points about these trees:

- **Entity source directories travel, but only for run time files.** On a host,
  `db/relational/books/` holds `.env`, `schema.sql` and `data/`. The QML is compiled into
  the binary, so it is not on the host.
- **`synqt.yaml` travels,** because the entities use the paths it spells. So does the
  profile, because each entity resolves the same layers the build did.
- **The data is not in `build/`.** A relational entity opens the file its `settings` name,
  in its own directory. So `synqt clean` cannot delete your database, and your backup job
  points at `db/relational/books/data/`, not at the build output.

## Step 2: Qt has to be there

`synqt build` runs no deployment step for service binaries, so a service host needs the
pinned Qt kit, at the path the build used or inside the image. There are two right ways
and one wrong way:

- **A container image built from the same base as your build machine.** The least
  surprising option, and the right one if you plan to use an orchestrator.
- **The same toolchain directory on the host.** Copy `synqt/toolchain/` with the rest, or
  run `synqt build` on the host once to fill it. Heavier, but needs no container runtime.
- **Never a distribution Qt of a nearby version.** The binaries were compiled against one
  Qt and load whatever the linker finds. A near miss fails in worse ways than a missing
  library.

The desktop client is the exception: it carries its own Qt, which the platform step in
[Cutting a release](tutorial-ship-release.md) bundles.

## Step 3: Read the start plan

Every build writes `build/process-manifest.json`, the input your process manager
needs:

```json
{
  "start_order": ["books", "edge"],
  "processes": [
    {
      "entity": "books",
      "binary": "build/books/books",
      "bind": "loopback",
      "mesh_cert": "synqt/mesh/books.crt",
      "mesh_key": "synqt/mesh/books.key",
      "ca_cert": "synqt/mesh/ca.crt"
    },
    {
      "entity": "edge",
      "binary": "build/edge/edge",
      "bind": "public",
      "mesh_cert": "synqt/mesh/edge.crt",
      "mesh_key": "synqt/mesh/edge.key",
      "ca_cert": "synqt/mesh/ca.crt"
    }
  ],
  "client_served_from": "build/client/"
}
```

It answers a supervisor's three questions:

- **`start_order`** lists owners before consumers. A consumer retries, so starting out of
  order is not fatal, but it turns a clean boot into a wait, and a first deploy into a
  debugging session about whether the link works.
- **`bind`** names the one entity that faces the public interface. If you ever find a
  second, the topology is wrong, not the host.
- **Each entry names the files that entity expects.** Check that list before deciding a
  start failure is a code problem; it usually is not.

## Step 4: Start it by hand, once

Before writing any service unit, check that the tree is right:

```cli
cd /srv/gavel
synqt doctor --profile production
```

`doctor` reports the resolved toolchain, which entities have a certificate, any selected
provider whose driver is missing, and your Qt license mode with its obligations. Fix
whatever it names. Then:

```cli
synqt serve --profile production
```

`synqt serve` starts each entity from the project root in manifest order, then returns.
Open the site, place a bid, close a lot, and check that the Hall of Fame remembers it.

`synqt serve` does not supervise: it does not restart an entity that dies, hence the next
step. It also passes `--dev` to nothing, which keeps the development sign-in and the
plaintext localhost link out of a deployment. Use it to bring up a staging machine by hand
and to check the tree works. Use a process manager for anything that must stay up.

## Step 5: Keep it alive

One unit per entity. On the database host, `/etc/systemd/system/gavel-books.service`:

```ini
[Unit]
Description=gavel books entity
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=gavel
Group=gavel
# Every path the entity reads is relative to the project root, so this line decides
# the deployment shape.
WorkingDirectory=/srv/gavel
ExecStart=/srv/gavel/build/books/books
Restart=on-failure
RestartSec=2

# The entity reads db/relational/books/.env itself, so systemd does not need to know the secrets.
# What it can do is make sure nothing else on the box can read them.
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/srv/gavel/db/relational/books/data

[Install]
WantedBy=multi-user.target
```

On the edge host, `/etc/systemd/system/gavel-edge.service` has the same shape with three
differences:

```ini
[Unit]
Description=gavel web edge
After=network-online.target gavel-books.service
Wants=network-online.target

[Service]
Type=simple
User=gavel
Group=gavel
WorkingDirectory=/srv/gavel
ExecStart=/srv/gavel/build/edge/edge
Restart=on-failure
RestartSec=2

NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
# Binding 443 without running as root.
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE

[Install]
WantedBy=multi-user.target
```

`After=` on the database unit follows `start_order`, and like the manifest it is only
advice: the edge would retry anyway. But a boot where the link comes up at once gives
logs you can read.

```cli
sudo systemctl enable --now gavel-books
sudo systemctl enable --now gavel-edge
```

## Step 6: Close the doors

The topology says the database is private. Make the network agree.

- **The public host exposes one port, the edge's.** If you can, expose neither the mesh
  port nor SSH to the world.
- **The database host exposes its mesh port only to the edge host,** through a security
  group, a firewall rule or a private subnet, whatever your hosting offers.
- **Mesh links use mutual TLS everywhere,** so a database reachable by accident is not
  immediately fatal. That is the second line of defense, not the first. See
  [network segmentation and the database](security.md#network-segmentation-and-the-database).

## Try it, then think

> [!QUESTION]
> A colleague wants to run the edge from `/usr/local/bin`, like a normal daemon. They copy
> `build/edge/edge` there, write a unit with no `WorkingDirectory`, and start it. It fails.
> Before reading on: what does it fail to find first, and why is that the right
> failure?

<details class="solution" markdown>
<summary>Solution</summary>

It fails on its topology. The entity reads `build/edge/topology.json` at startup to learn
what it owns, what it consumes and where its peers are, and it looks for the file relative
to where it started. From `/`, that path does not exist.

Past that, it would fail on the certificate, then the bundle, then the env file: four
failures in a row with one cause.

Fix it by setting the unit's `WorkingDirectory` to the project root, not by making the
paths absolute, because the project root is the deployment. Everything an entity needs is
described relative to it, in one readable file, so you can look at a host and see the
whole system.

To have the binary on a path, symlink it. It still runs with the working directory the
unit sets.

</details>

## Advice worth taking now

- **Back up `db/relational/books/data/`, not `build/`.** A commit reproduces the build;
  nothing reproduces the data.
- **Use the same project root path on every host.** `/srv/gavel` on both means one unit
  template and one runbook.
- **Log to the journal.** The entities write to standard error, and systemd captures it.
  Do not add file logging without a reason; the reason usually turns out to be a missing
  metric, not a missing file.
- **Keep the previous release directory.** Make the project root
  `/srv/gavel-2026-08-03/`, with `/srv/gavel` a symlink to it, so a rollback means moving
  the symlink and restarting. [Cutting a release](tutorial-ship-release.md) builds on
  this.
- **Run the [security checklist](security.md#security-checklist)** before you call it
  done. It is short, and meant for deploy time.

Next: [Cutting a release](tutorial-ship-release.md), and what changes on the second
deploy.
