<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# synqt

The command line tool for [SynQt](https://synqt.org/), a framework for building complete
web systems in Qt and QML with no third party servers to stand up.

`synqt` scaffolds a project, resolves and pins its toolchain (the Qt SDK and the
Emscripten compiler), builds every entity in it (the browser client to WebAssembly, the
services natively), issues the project's mesh certificates, validates the topology, and
runs the whole system locally with file watching and hot reload.

**Documentation, tutorials, and the full reference: [synqt.org](https://synqt.org/).**

## Install

```sh
pipx install synqt
```

`pip install synqt` works too; `pipx` is the recommendation only because this is an
application rather than a library. There is also a single-file binary that needs no
Python at all, if you would rather have that:

```sh
curl -fsSL https://get.synqt.org/install.sh | sh   # macOS and Linux
irm https://get.synqt.org/install.ps1 | iex        # Windows, in PowerShell
```

Both are cut from the same tag and behave identically.

## Use

```sh
synqt create        # scaffold a project, asking a short set of questions
cd my-app
synqt dev           # build, serve, watch, and open a browser
```

`synqt new my-app --auth github --blueprint persistence` is the same scaffolder without
the questions, for a script. `synqt doctor` reports what is installed and what is
missing, and is the first thing to run when something will not build.

The full command reference is in [build system and
CLI](https://synqt.org/build-system-and-cli/); the walkthrough is in [getting
started](https://synqt.org/getting-started/).

## What this package does not include

Only `synqt` itself is Python. Everything it builds is Qt: the first `synqt dev` or
`synqt build` in a project downloads and pins the Qt SDK and the Emscripten toolchain
into that project, so every machine and every teammate compiles with the same versions.
You never install Qt or Emscripten yourself, but the first build does take a few minutes
and does need the disk space.

The framework's own C++ and CMake sources ship inside this package, so a `pipx`-installed
`synqt` can scaffold and build without a SynQt checkout. Setting `SYNQT_ROOT` to a
checkout overrides them, which is what you want when working on SynQt itself.

## Licensing

`synqt`, and all of SynQt's own source, is Apache-2.0. What you *build* with it inherits
Qt's license instead: under open source Qt the browser client is GPLv3 and is conveyed to
every visitor, so its source has to be published; a commercial Qt license removes that.
`synqt new`, `synqt build --release` and `synqt doctor` each say so at the point it
matters, and [licensing](https://synqt.org/licensing/) is the full analysis.
