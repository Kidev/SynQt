# monitor-console

The monitoring console, end to end, in a real browser.

Everything this suite asserts is invisible to a compiler:

- a bundle outside the caller's scope answers 404 rather than 403, so it is not addressable
- the sign-in gate is a page with a form, and its inline script has to survive the edge's
  strict Content-Security-Policy
- signing in raises the same session, so the same URL returns a different bundle
- the console is a Qt Quick scene that either draws or does not
- the console reached the monitor, which the monitor's own record proves

The scaffolder writes the project when the suite runs
([`make-project.py`](make-project.py)) rather than the suite checking one in, so what
loads is what `synqt add entity <name> --type monitor` produces today.

## Running it

```console
$ bash tests/monitor-console/run-monitor-console.sh
```

Phases 1 and 2 (scaffold, build the monitor and the reporting edge) need only the host kit.
Phases 3 and 4 need the WebAssembly kit at `$QT_WASM` and a browser runtime; without the
kit the suite says so and stops rather than failing on a toolchain the host was never
given.

## What it found

Written after the monitoring feature was complete and every other suite was green. It found
seven defects, four of them outside monitoring:

| What | Where |
| --- | --- |
| The monitor's bundle gate was wired to entity names, so the console could never be delivered | `maingen.render_monitor_main`, `run.dev_command` |
| The reporting path called into a Replica across threads (`Timers cannot be stopped from another thread`) | `IngestClient::send` |
| `Server` did not resolve for a console client, though the monitor is that client's edge | `maingen.render_client_main` |
| The scaffolded console's attached handler named the owner instead of the contract, so the QML would not load at all | `monitorscaffold.console_qml` |
| Every console session stayed connected to the process-wide service after its Source was gone | `monitorentity.CONSOLE_SOURCE_QML` |
| A scaffolded monitor took the port the edge already had | `monitorscaffold.free_port`, plus a `synqt check` rule |
| **Open.** An edge can abort an idle keep-alive HTTP connection and record it as a refused upgrade | `WebEdge::trackPendingUpgrade`; see below |

## The one it found and did not fix

`WebEdge` arms a handshake-deadline timer on every accepted socket and cancels it only when
a WebSocket upgrade arrives. An ordinary HTTP connection therefore has
`security.handshake_timeout_ms` to live whatever it is doing: past that the socket is
aborted and `upgradeRejected("handshake timeout")` is emitted. A browser that keeps a
connection alive between fetches, or a single response that takes longer than the deadline,
is cut off by the server it is talking to, and the operations record gains a refusal nobody
made.

It was seen here: an early run of this suite recorded four
`upgrade refused: handshake timeout` events per session from a browser that was only
loading the console. It is timing-dependent, so a given run may show none.

Four fixes were tried and every one of them pushed
[`tests/memory`](../memory)'s `theEdgeLetsGoOfABrowserThatComesAndGoes` from comfortably
inside its 64-bytes-per-cycle budget to 660-1020 bytes per browser connect/disconnect:
cancelling the timer from the after-request handler, restarting it there, stopping it
without deleting it, and moving the check onto the socket with a `readyRead` watch that
touches the request path not at all. The last of those adds one signal connection and no
objects, which is not an explanation for 800 bytes, so the cause is not understood. The fix
is written down here rather than shipped on a guess, and closing it means understanding
what that budget is actually measuring first.
