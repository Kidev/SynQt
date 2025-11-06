<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Quick start

Four commands, from nothing to a system running in your browser.

```cli
curl -fsSL https://get.synqt.org/install.sh | sh
synqt new app
cd app
synqt dev
```

On Windows the first line is `irm https://get.synqt.org/install.ps1 | iex`, and if you
already have Python, `pipx install synqt` gets you the same CLI.

`synqt dev` builds every entity, brings them all up, and opens the client. Edit any `.qml`
file and the running system picks it up.

## Or draw it first

If you would rather see the shape of a system before typing anything, open the
[editor](/designer/). Draw the entities and the links between them, press Download, and
unzip the result over a project made with `synqt new`. Nothing is installed and nothing
leaves the page. The [guide to the editor](visual-editor.md) covers what it can do.

## Where to go next

- [Getting started](getting-started.md) walks the same ground slowly, and explains what
  each command did.
- [The auction tutorial](tutorial.md) builds a real system end to end: live bidding, then
  sign-in, then a database the browser cannot reach.
- [Architecture](architecture.md) is the reference, from the entity model to the security
  design.
