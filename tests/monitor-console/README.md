<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# monitor-console

The monitoring console, end to end, in a real browser.

Everything this suite asserts is invisible to a compiler:

- a bundle outside the caller's scope answers 404 and not 403, so it is not addressable
- the sign-in gate is a page with a form, and its inline script has to survive the edge's
  strict Content-Security-Policy
- signing in raises the same session, so the same URL returns a different bundle
- the console is a Qt Quick scene that either draws or does not
- the console reached the monitor, which the monitor's own record proves

The scaffolder writes the project when the suite runs
([`make-project.py`](make-project.py)), and the suite checks no project in, so what loads
is what `synqt add entity <name> --type monitor` produces today.

## Running it

```sh
bash tests/monitor-console/run-monitor-console.sh
```

Phases 1 and 2 (scaffold, build the monitor and the reporting edge) need only the host
kit. Phases 3 and 4 need the WebAssembly kit at `$QT_WASM` and a browser runtime. Without
the kit the suite says so and stops, and it does not fail on a toolchain the host was
never given.

## What it guards

Seven failure modes, four of them outside monitoring, that a compiler and the other suites
cannot see:

| Failure | Where |
| --- | --- |
| A monitor bundle gate wired to entity names, so the console is never delivered | `maingen.render_monitor_main`, `run.dev_command` |
| A reporting path that calls into a Replica across threads (`Timers cannot be stopped from another thread`) | `IngestClient::send` |
| `Server` not resolving for a console client, though the monitor is that client's edge | `maingen.render_client_main` |
| A scaffolded console whose attached handler names the owner instead of the contract, so the QML does not load | `monitorscaffold.console_qml` |
| A console session that stays connected to the process-wide service after its Source is gone | `monitorentity.CONSOLE_SOURCE_QML` |
| A scaffolded monitor that takes the port the edge already has | `monitorscaffold.free_port`, plus a `synqt check` rule |
| An edge that aborts an idle keep-alive HTTP connection and records it as a refused upgrade | `WebEdge::trackPendingUpgrade`, see below |

## The handshake deadline

A handshake deadline armed on every accepted socket and cancelled only when a WebSocket
upgrade arrives gives an ordinary HTTP connection `security.handshake_timeout_ms` to live,
whatever it is doing. Past that the edge aborts the socket and emits
`upgradeRejected("handshake timeout")`. A browser keeping a connection alive between
fetches, or a single response slower than the deadline, is then cut off by the server it
is talking to, and the operations record gains a refusal nobody made. A browser that is
only loading the console produces four of them per session.

The deadline ends at the first byte the peer sends, which leaves it bounding what it is
for: a socket that connects and stays silent.
`tests/webedge`'s `aKeepAliveConnectionThatFetchedThePageIsNotClosedUnderIt` is the
regression guard, and it fails without that behaviour.

[`tests/memory`](../memory)'s `theEdgeLetsGoOfABrowserThatComesAndGoes` measures what a
connection leaves behind, and it has to measure a slope. Comparing the heap after N cycles
against the heap before them, and requiring the difference to be near zero, fails a build
that retains nothing per connection, because the heap under that workload is not a
straight line. It drops about 228 KB in one move partway through a long
run and ends 200 KB below where it started, which reads as 660-1020 bytes per connection
against a 64-bytes-per-cycle budget. The suite measures the slope between two equal windows
instead, and `theBudgetCanTellALeakFromABusyProcess` leaks a known amount on purpose to
prove the budget can still come back negative.
