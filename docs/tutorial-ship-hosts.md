<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Where the binaries go

There is one thing to understand before any of the commands on this page make sense:

**A SynQt deployment is a project directory, not a binary.**

Every entity resolves its runtime files relative to the directory it was started from,
exactly as they are spelled in `synqt.yaml`. Its topology under `build/<entity>/`, its
certificate under `synqt/mesh/`, its secrets in its own `.env`, its schema and its data
in its own folder, and, for the edge, the client bundle under `build/client/`. Copy
`build/web/web` somewhere on its own and it starts, looks for all of that, and finds
none of it.

Once that clicks, the rest of this page is bookkeeping.

## Step 1: The shape on each host

Take the artifact your pipeline produced and prune it to what each host actually runs.

The edge host:

```text
/srv/gavel/
  synqt.yaml
  synqt.production.yaml
  certs/web/fullchain.pem
  certs/web/privkey.pem
  synqt/mesh/
    ca.crt
    web.crt
    web.key
  build/
    web/              # the edge binary and its topology.json
    client/           # the bundle it serves
  web/
    .env              # the OAuth client secret
```

The database host:

```text
/srv/gavel/
  synqt.yaml
  synqt.production.yaml
  synqt/mesh/
    ca.crt
    database.crt
    database.key
  build/
    database/         # the binary and its topology.json
  database/
    .env
    schema.sql
    data/             # the SQLite file lives here
```

Three things about those trees.

**The entity source directories travel, but only for what an entity reads at run time.**
`database/` on a host means `.env`, `schema.sql` and `data/`. It does not mean the QML,
which is compiled into the binary and is not on the host at all.

**`synqt.yaml` travels** because the paths the entities use are the paths it spells. So
does the profile, because the entity resolves the same layering the build did.

**The data is not in `build/`.** A persistence entity opens the file its `settings` name,
under its own directory, which is why `synqt clean` cannot take your database with it and
why your backup job points at `database/data/` rather than at the build output.

## Step 2: Qt has to be there

`synqt build` does not run a deployment step for service binaries, so **a service host
needs the pinned Qt kit present**, at the same path the build used or baked into the
image. Two ways to get that right, and one way to get it wrong:

- **A container image built from the same base as your build machine.** Least surprising,
  and the answer if you are going anywhere near an orchestrator later.
- **The same toolchain directory on the host.** Copy `synqt/toolchain/` along with the
  rest, or run `synqt build` on the host once to populate it. Heavier, but it needs no
  container runtime.
- **Not this:** a host with a distribution Qt of a nearby version. The binaries were
  compiled against one Qt and will load whatever the linker finds, and the failures from
  a near miss are worse than the failure from an absence.

The desktop client is the exception, and it is genuinely an exception: it carries its own
Qt, because [Cutting a release](tutorial-ship-release.md) runs the platform step that
puts it there.

## Step 3: Read the start plan

Every build writes `build/process-manifest.json`. It is not documentation, it is the
input your process manager wants:

```json
{
  "start_order": ["database", "web"],
  "processes": [
    {
      "entity": "database",
      "binary": "build/database/database",
      "bind": "loopback",
      "mesh_cert": "synqt/mesh/database.crt",
      "mesh_key": "synqt/mesh/database.key",
      "ca_cert": "synqt/mesh/ca.crt"
    },
    {
      "entity": "web",
      "binary": "build/web/web",
      "bind": "public",
      "mesh_cert": "synqt/mesh/web.crt",
      "mesh_key": "synqt/mesh/web.key",
      "ca_cert": "synqt/mesh/ca.crt"
    }
  ],
  "client_served_from": "build/client/"
}
```

It answers the three questions a supervisor has.

`start_order` is owners before consumers. A consumer retries, so starting out of order is
not fatal; it turns a clean boot into a wait, and turns a first deploy into a debugging
session about whether the link works.

`bind` says which single entity faces the public interface. Exactly one does. If you ever
find a second, something is wrong with the topology and not with the host.

And each entry names the material that entity expects, which is the list to check before
concluding that a start failure is a code problem. It usually is not.

## Step 4: Start it by hand, once

Before writing any service unit, prove the tree is right:

```cli
cd /srv/gavel
synqt doctor --profile production
```

`doctor` reports the resolved toolchain, which entities have a certificate and which do
not, any selected provider whose driver is missing, and which Qt licence mode you are in
along with what it obliges. Fix whatever it names. Then:

```cli
synqt serve --profile production
```

`synqt serve` starts each entity from the project root in manifest order and returns. Open
the site. Take a bid. Close a lot and confirm the Hall of Fame remembers it.

Two things `synqt serve` is not. It does **not supervise**: it will not restart an entity
that dies, which is why the next step exists. And it passes **no `--dev` flag** to
anything, which is what keeps the development stub identity provider out of a running
deployment. Use it to bring a staging box up by hand and to answer "does this tree work
at all"; use a process manager for anything that has to stay up.

## Step 5: Keep it alive

One unit per entity. On the database host, `/etc/systemd/system/gavel-database.service`:

```ini
[Unit]
Description=gavel database entity
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=gavel
Group=gavel
# The whole of the deployment shape in one line: every path the entity reads is
# relative to the project root, so this is not a detail.
WorkingDirectory=/srv/gavel
ExecStart=/srv/gavel/build/database/database
Restart=on-failure
RestartSec=2

# The entity reads database/.env itself, so systemd does not need to know the secrets.
# What it can do is make sure nothing else on the box can read them.
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/srv/gavel/database/data

[Install]
WantedBy=multi-user.target
```

On the edge host, `/etc/systemd/system/gavel-web.service`, the same shape with three
differences:

```ini
[Unit]
Description=gavel web edge
After=network-online.target gavel-database.service
Wants=network-online.target

[Service]
Type=simple
User=gavel
Group=gavel
WorkingDirectory=/srv/gavel
ExecStart=/srv/gavel/build/web/web
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

`After=` on the database unit encodes `start_order`, and it is advisory in the way the
manifest describes: the edge would retry anyway, but a boot where the link is up
immediately is a boot you can read.

```cli
sudo systemctl enable --now gavel-database
sudo systemctl enable --now gavel-web
```

## Step 6: Close the doors

The topology says the database is private. The network should agree.

- **The public host exposes one port**, the edge's. Not the mesh port, not SSH to the
  world if you can help it.
- **The database host exposes its mesh port to the edge host only.** A security group, a
  firewall rule, a private subnet; whichever your hosting gives you.
- **Mesh links are mutual TLS wherever they run**, so a database reachable by accident is
  not immediately fatal. That is a second line, not the first one. See [network
  segmentation and the database](security.md#network-segmentation-and-the-database).

## Try it, then think

> [!QUESTION]
> A colleague wants to run the edge from `/usr/local/bin`, the way a normal daemon
> works. They copy `build/web/web` there, write a unit with no `WorkingDirectory`, and
> start it. It fails. Before reading on: what does it fail to find first, and why is
> that the right failure?

<details class="solution" markdown>
<summary>Solution</summary>

It fails on its topology. `build/web/topology.json` is the file the entity reads at
startup to learn what it owns, what it consumes, and where its peers are, and it looks
for it at a path relative to where it was started. From `/`, that path does not exist.

Had it got past that, the next failure would have been the certificate, then the bundle,
then the env file: four failures in a row that all mean the same thing.

The right fix is not to make the paths absolute. It is that the unit sets
`WorkingDirectory` to the project root, because the project root is the deployment.
Everything an entity needs is described relative to it, in one file a person can read,
and that is what makes a deployment inspectable: you can look at a host and see the whole
system, rather than a binary and a hope.

If you want the binary on a path, symlink it. The link's target still runs with whatever
working directory the unit sets.

</details>

## Advice worth taking now

- **Back up `database/data/`, not `build/`.** The build is reproducible from a commit.
  The data is not reproducible from anything.
- **Give each host the same project root path.** `/srv/gavel` on both means one unit
  template, one runbook, and one place your muscle memory takes you.
- **Log to the journal and leave it there.** The entities write to standard error;
  systemd captures it. Resist the urge to add file logging before you have a reason,
  because the reason usually turns out to be a missing metric rather than a missing file.
- **Keep the previous release directory.** `Where the binaries go` becomes
  `/srv/gavel-2026-08-03/` with `/srv/gavel` a symlink to it, and a rollback becomes
  moving the symlink and restarting. [Cutting a release](tutorial-ship-release.md) picks
  that up.
- **Run the [security checklist](security.md#security-checklist-use-before-every-deploy)
  before you call it done.** It is short, and it is written to be read at deploy time
  rather than at design time.

Next: [Cutting a release](tutorial-ship-release.md), and what changes when the second
deploy happens.
