<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The pipeline that says no

A pipeline earns its keep by refusing things. Building on every push is easy and
almost worthless on its own: what you want is for the push that would have broken
production to stop at the pipeline, with the reason on screen, before anyone has to
think about rolling anything back.

SynQt gives you most of that for free, because the rules that make a deployment safe
are already written down. They live in your `synqt.yaml`, and there is a command that
holds you to them. This page turns those commands into a workflow.

Everything here runs on your machine first. Run each command locally before you put it
in CI, so that the first red pipeline is a real failure rather than a typo in a YAML
file.

## Step 1: Say what production is different about

Your `gavel` project has one topology. Production changes a handful of values in it:
the port, the TLS certificate, and the address the database answers on. Do not copy
the file. Put the differences in a profile next to it.

Create `synqt.production.yaml` in the project root:

```yaml
# synqt.production.yaml
# Applied with: --profile production. Only the keys that differ from synqt.yaml.
public:
  port: 443
  tls:
    cert_file: certs/web/fullchain.pem
    key_file: certs/web/privkey.pem

entities:
  - name: web
    public:
      origin: https://gavel.example.com
    mesh:
      host: 10.0.0.10

  - name: database
    mesh:
      host: 10.0.0.20
```

Two things about that file are worth more than the values in it.

It **adds and changes, and never removes**. There is no syntax for dropping a consumer
or an entity, because removing one is a security change and belongs in the file that
declares the list. A profile you can read in ten seconds cannot quietly widen anything.

And it holds **no secrets**. The database password is not here and will not be here;
secrets come from a per entity env file and nowhere else, which
[Two authorities](tutorial-ship-certificates.md) covers. If you are tempted to put one
in, the validator will refuse it, which is the point.

The full layering rules are in [configuration resolution
order](project-layout-and-config.md#configuration-resolution-order).

## Step 2: Ask the production question

```cli
synqt check --release --profile production
```

Plain `synqt check` validates the system you are developing. It has to allow a
localhost topology, because that is what you have been running all tutorial. `--release`
adds the rules that bind only a system you ship:

- the web edge must terminate TLS itself or declare that something in front of it does,
- a mesh link that crosses hosts may not drop mutual TLS,
- a desktop client's `edge_url` must be `wss://`,
- an external provider may not connect in plaintext.

Run it now and read what it says. If you wrote the profile above and your `synqt.yaml`
is the auction's, it passes. Then break it on purpose: delete the `tls:` block from
`synqt.production.yaml` and run it again.

```text
error: entity "web" faces the internet with no TLS: add a tls block
       (cert_file, key_file) or set public.tls_terminated_upstream: true
```

There is no third state and no default. A framework that guessed here would be
guessing about whether your users' traffic is encrypted. Put the block back.

One rule deliberately does **not** fire here: a missing mesh certificate. Certificates
are issued from a private key that is not supposed to exist on a build machine, so that
check happens when entities start, not when they build. That asymmetry is the shape of
the next page.

## Step 3: The other three commands

```cli
synqt test
synqt build --release --profile production
```

`synqt test` builds and runs the project's own QML tests, the ones that load a connect
point on its own and drive its slots as a chosen caller. If the auction has none yet,
this is the moment to write one, because a pipeline that only builds tells you your
code compiles. [Testing an application](testing.md) is the how.

`synqt build --release` compiles every entity through the pinned toolchain and writes
one directory per entity under `build/`, plus `build/process-manifest.json`, the start
plan. It also prints something you should read rather than skim: under open source Qt,
your client is GPLv3 and is conveyed to every visitor, so its source must be offered.
[Cutting a release](tutorial-ship-release.md) comes back to that.

Confirm what you got:

```cli
ls build
```

```text
client/  database/  process-manifest.json  web/
```

## Step 4: Write the workflow

Now the same four commands, on a machine that is not yours. Create
`.github/workflows/ship.yml`:

```yaml
name: "[SHIP] Check, test and build gavel"

on:
  push:
  pull_request:

jobs:
  build:
    runs-on: ubuntu-latest
    timeout-minutes: 45
    steps:
      - uses: actions/checkout@v7

      - name: Install the synqt CLI
        run: curl -fsSL https://get.synqt.org/install.sh | sh

      # The toolchain is pinned by project.qt_version, so this key changes only when
      # you deliberately move the project to another Qt. Without it every run
      # downloads a Qt kit and an Emscripten toolchain to produce the same bytes.
      - name: Cache the pinned toolchain
        uses: actions/cache@v6
        with:
          path: synqt/toolchain
          key: synqt-toolchain-${{ hashFiles('synqt.yaml') }}

      - name: Validate the system as it will be deployed
        run: synqt check --release --profile production

      - name: Run the project's tests
        run: synqt test

      - name: Build every entity
        run: synqt build --release --profile production

      - name: Keep the artifact
        uses: actions/upload-artifact@v7
        with:
          name: gavel-${{ github.sha }}
          path: |
            build/
            synqt.yaml
            synqt.production.yaml
            web/.env.example
            database/schema.sql
```

Four things in there are worth saying out loud.

**The check runs first.** It takes seconds and it catches the failures that would
otherwise cost you a build. Ordering a pipeline cheapest-first is not a
micro-optimisation, it is what decides whether people wait for it.

**The toolchain is cached on the configuration, not on the lockfile of the week.**
`project.qt_version` in `synqt.yaml` is what pins Qt and, through it, Emscripten. Key
the cache on that file and a run either reuses the exact kit the last one used or
rebuilds because you changed the pin on purpose.

**The artifact is the project shape, not just the binaries.** A SynQt deployment is a
directory whose parts find each other by relative path, so an artifact that carries
only `build/` is one you cannot start. [Where the binaries
go](tutorial-ship-hosts.md) is about exactly that shape; the upload list above is its
short version.

**Nothing in this workflow can issue a certificate.** There is no CA key in the
repository and none in the secrets. A pipeline that could mint a mesh identity is a
pipeline that can impersonate any entity in your system, and CI is the part of your
infrastructure with the most people and the most third party code in it. Issuing stays
manual and stays elsewhere.

## Try it, then think

> [!QUESTION]
> Your pipeline builds and your tests pass, so the system is safe to deploy. Someone
> opens a pull request that adds the client to the ledger connect point's consumers:
>
> ```
> consumers = ["web", "client"]
> ```
>
> The QML compiles. The tests pass, because none of them looks at the database from
> the browser. Predict what the pipeline does.

<details class="solution" markdown>
<summary>Solution</summary>

The `synqt check` step fails, and it fails first, before anything is compiled. A
connect point the browser consumes must be owned by a web edge, and the database is
not one.

This is why the check step exists as a step. Tests answer "does the code do what its
author meant"; the check answers "is this system one that may be deployed at all", and
those are different questions with different failure modes. A topology mistake usually
compiles perfectly and usually passes the tests, because the tests were written by the
same person who made the mistake.

The full list of what the check refuses, with and without `--release`, is under
[validation](project-layout-and-config.md#validation).

</details>

## Advice worth taking now

- **Pin the Qt version and mean it.** `project.qt_version` is the single value that
  decides which compiler produced your binaries. Changing it is a deliberate act with
  its own pull request, not something that drifts because a runner image moved.
- **Build once, deploy that.** The artifact your pipeline produced is what goes to
  staging and then to production. Rebuilding for production because staging passed
  means the thing you tested is not the thing you shipped.
- **Name artifacts by commit.** `gavel-${{ github.sha }}` above. When someone asks
  what is running on the edge host, you want the answer to be a commit and not a date.
- **Do not put deployment in this workflow yet.** A pipeline that can reach production
  is a different security question from a pipeline that produces a file. Get the file
  right first; the next two pages are what makes deploying it boring enough to
  automate.

Next: [Two authorities](tutorial-ship-certificates.md), and the key that never comes
near the pipeline you just wrote.
