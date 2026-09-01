<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The pipeline that says no

A pipeline is useful because it refuses things. Building on every push is easy and worth
little alone. You want the push that would break production to stop in the pipeline, with
the reason on screen, before anyone has to think about a rollback.

SynQt already provides most of this: the rules that make a deployment safe are in your
`synqt.yaml`, and a command enforces them. This page turns those commands into a
workflow.

Run each command locally before putting it in CI, so a typo shows up on your machine and
the first red pipeline is a real failure.

## Step 1: Say what production is different about

Your `gavel` project has one topology. Production changes a few of its values: the port
the edge listens on, the address browsers reach it at, and where each entity is reached on
the private network. Do not copy the file; put the differences in a profile beside it.

Create `synqt.production.yaml` in the project root:

```yaml
# synqt.production.yaml
# Applied with: --profile production. Only the keys that differ from synqt.yaml.
entities:
  - name: edge
    public:
      port: 443
      origin: https://gavel.example.com
    mesh:
      host: 10.0.0.10

  - name: books
    mesh:
      host: 10.0.0.20
```

An entity in a profile is matched by its `name`, and only the keys written under it change.
The edge keeps the `tls:` block `synqt.yaml` gives it, and every other entity is untouched.

Two properties of this file matter more than its values.

**It adds and changes, and never removes.** There is no syntax for dropping a consumer or
an entity: removing one is a security change and belongs in the file that declares the
list. A profile you can read in ten seconds cannot quietly widen anything.

**It holds no secrets.** The database password is not here and never will be. Secrets come
only from a per entity env file, covered in
[Two authorities](tutorial-ship-certificates.md). The validator refuses a secret in a
profile.

The full layering rules are in [configuration resolution
order](project-layout-and-config.md#configuration-resolution-order).

## Step 2: Ask the production question

```cli
synqt check --release --profile production
```

Plain `synqt check` validates the system you are developing, so it allows a localhost
topology, which is what you have run so far. `--release` adds the rules that apply only
to a system you ship:

- the web edge must terminate TLS itself or declare that something in front of it does,
- a mesh link that crosses hosts may not drop mutual TLS,
- a desktop client's `edge_url` must be `wss://`,
- an external provider may not connect in plaintext.

Run it now and read the output. With the profile above and the auction's `synqt.yaml`, it
passes. Then break it on purpose: delete the edge's `tls:` block from `synqt.yaml` and run
it again.

```text
error: web edge 'edge' has no tls section, so a release build would serve the browser
over plaintext; give it tls.cert_file and tls.key_file, or set
public.tls_terminated_upstream: true if a reverse proxy in front of it terminates TLS
```

There is no third option and no default: guessing here would mean guessing whether your
users' traffic is encrypted. Put the block back.

One rule does not fire here: a missing mesh certificate. Certificates come from a private
key that must not exist on a build machine, so that check runs when entities start, not
when they build. The next page covers it.

## Step 3: The other three commands

```cli
synqt test
synqt build --release --profile production
```

`synqt test` builds and runs the project's QML tests, which load one connect point alone
and drive its slots as a chosen caller. If the auction has none yet, write one now: a
pipeline that only builds only tells you the code compiles. See
[testing an application](testing.md).

`synqt build --release` compiles every entity with the pinned toolchain and writes one
directory per entity under `build/`, plus `build/process-manifest.json`, the start plan.
Read what it prints: under open source Qt your client is GPLv3 and is conveyed to every
visitor, so you must offer its source. [Cutting a release](tutorial-ship-release.md)
returns to this.

Confirm what you got:

```cli
ls build
```

```text
client/  books/  edge/  process-manifest.json
```

## Step 4: Write the workflow

Now run the same four commands on a CI machine. It starts with no Qt, and `synqt` does
not download Qt. The provisioning step below runs the commands `synqt doctor` prints on a
bare machine; they install into `synqt/toolchain/`, which the cache step keeps between
runs. Create `.github/workflows/ship.yml`:

```yaml
name: "[SHIP] Check, test and build gavel"

on:
  push:
  pull_request:

jobs:
  build:
    # A named image rather than ubuntu-latest, so the machine this runs on next year is
    # the machine it runs on today. Move it when you decide to rather than when GitHub does.
    runs-on: ubuntu-24.04
    timeout-minutes: 45
    steps:
      - uses: actions/checkout@v7

      - name: Install the synqt CLI
        run: curl -fsSL https://get.synqt.org/install.sh | sh

      # The toolchain is pinned by project.qt_version, so this key changes only when
      # you move the project to another Qt. Without it every run installs a Qt kit and
      # an Emscripten toolchain to produce the same bytes.
      - name: Cache the pinned toolchain
        id: toolchain
        uses: actions/cache@v6
        with:
          path: synqt/toolchain
          key: synqt-toolchain-${{ hashFiles('synqt.yaml') }}

      # The commands `synqt doctor` prints when nothing is installed: the host kit with
      # the four add-on modules SynQt links, the WebAssembly kit, QtRemoteObjects built
      # from source for it (no prebuilt one exists), and the Emscripten the pin names.
      - name: Provision the pinned toolchain
        if: steps.toolchain.outputs.cache-hit != 'true'
        run: |
          sudo apt-get install -y ninja-build
          pip install aqtinstall
          aqt install-qt linux desktop 6.12.0 linux_gcc_64 \
            -m qthttpserver qtnetworkauth qtremoteobjects qtwebsockets -O synqt/toolchain/qt
          aqt install-qt all_os wasm 6.12.0 wasm_singlethread -m qtwebsockets -O synqt/toolchain/qt
          aqt install-src linux 6.12.0 --archives qtremoteobjects --outputdir synqt/toolchain/qt
          git clone https://github.com/emscripten-core/emsdk synqt/toolchain/emsdk
          synqt/toolchain/emsdk/emsdk install 5.0.5
          synqt/toolchain/emsdk/emsdk activate 5.0.5
          source synqt/toolchain/emsdk/emsdk_env.sh
          QT_HOST_PATH=$PWD/synqt/toolchain/qt/6.12.0/gcc_64 \
            synqt/toolchain/qt/6.12.0/wasm_singlethread/bin/qt-cmake \
            -S synqt/toolchain/qt/6.12.0/Src/qtremoteobjects -B build/qtro-wasm -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX=$PWD/synqt/toolchain/qt/6.12.0/wasm_singlethread
          cmake --build build/qtro-wasm && cmake --install build/qtro-wasm

      - name: Confirm the toolchain resolves
        run: synqt doctor

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
            web/edge/.env.example
            db/relational/books/schema.sql
```

Five points matter:

- **The workflow provisions the toolchain, not `synqt`.** The CLI finds what is installed
  and pins the versions, and `synqt doctor` prints the exact commands for what is
  missing. The provisioning step runs those commands, and `synqt doctor` after it checks
  that they installed where the build looks.
- **The check runs first.** It takes seconds and catches failures that would otherwise
  cost a build. Putting the cheapest step first keeps people from waiting.
- **The cache key is the configuration.** `project.qt_version` in `synqt.yaml` pins Qt,
  and through it Emscripten. Key the cache on that file, and a run either reuses the last
  kit or rebuilds because you changed the pin on purpose.
- **The artifact is the project layout, not just the binaries.** A SynQt deployment is a
  directory whose parts find each other by relative path, so an artifact with only
  `build/` cannot start. [Where the binaries go](tutorial-ship-hosts.md) describes that
  layout; the upload list above is the short version.
- **Nothing in this workflow can issue a certificate.** There is no CA key in the
  repository or in the secrets. A pipeline that could create a mesh identity could
  impersonate any entity in your system, and CI is the part of your infrastructure with
  the most people and the most third party code. Issuing stays manual, and happens
  elsewhere.

## Try it, then think

> [!QUESTION]
> Your pipeline builds and your tests pass, so the system seems safe to deploy. Someone
> opens a pull request that adds the client to the books entity's consumer list:
>
> ```yaml
> consumers: [edge, app]
> ```
>
> The QML compiles, and the tests pass, because none of them reaches the database from
> the browser. Predict what the pipeline does.

<details class="solution" markdown>
<summary>Solution</summary>

The `synqt check` step fails first, before anything compiles: a web edge must own any
connect point the browser consumes, and the database is not a web edge.

That is why the check is a separate step. Tests answer "does the code do what its author
meant?" The check answers "may this system be deployed at all?" These questions fail in
different ways. A topology mistake usually compiles and passes the tests, because the same
person wrote the tests and made the mistake.

[Validation](project-layout-and-config.md#validation) lists everything the check refuses,
with and without `--release`.

</details>

## Advice worth taking now

- **Pin the Qt version.** `project.qt_version` alone decides which compiler built your
  binaries. Change it in its own pull request, never by drift because a runner image
  moved.
- **Build once, deploy that build.** The artifact the pipeline produced goes to staging,
  then to production. If you rebuild for production after staging passes, what you tested
  is not what you shipped.
- **Name artifacts by commit,** as `gavel-${{ github.sha }}` does above. When someone asks
  what runs on the edge host, the answer should be a commit, not a date.
- **Keep deployment out of this workflow for now.** A pipeline that can reach production
  raises different security questions than one that produces a file. Get the file right
  first; the next two pages make deploying it simple enough to automate.

Next: [Two authorities](tutorial-ship-certificates.md), and the key that never comes near
this pipeline.
