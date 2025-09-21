<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# QtRemoteObjects on Qt for WebAssembly: a returning slot never resolves in Firefox on a headless Linux CI runner

Report prepared for upstream Qt. Everything below is measured, and each claim names the run
or the probe it came from. Where a thing was suspected and then ruled out, it is listed as
ruled out rather than dropped, because the ruled-out set is what makes the remaining
mechanism the only one left.

## Summary

A returning slot called from a Qt for WebAssembly client over QtRemoteObjects-on-QtWebSockets
never resolves in Firefox on GitHub's hosted Ubuntu runner. The reply is not lost. It arrives,
it decodes, its serial matches the pending call, and `QConnectedReplicaImplementation::notifyAboutReply`
sets `error = NoError` and the return value on the call. What never happens is the delivery of
`QRemoteObjectPendingCallWatcher::finished`, which is the only way a caller learns the call
finished without blocking. That signal is emitted across a `Qt::QueuedConnection`, so it needs
a `QEvent::MetaCall` to be posted and drained, and in this environment it is not drained for
that object.

Everything else on the same socket keeps working for the whole timeout window: property
pushes, signals, model updates, and the QtRO heartbeat. So the transport is alive, the
connection is healthy, and only the one path that depends on a queued meta-call is dead.

The same client passes in Chromium and WebKit on that runner, in Firefox on the macOS runner,
in Playwright's Firefox on a developer machine, and in a stock Firefox driven over WebDriver
BiDi. The failure is specific to Firefox plus that runner environment.

## Environment

| | |
|---|---|
| Qt | 6.11.1 (prebuilt `all_os`/`wasm` single-threaded kit) |
| QtRemoteObjects | 6.11.1, built from the pinned source into the WASM kit (the prebuilt kit ships no QtRemoteObjects) |
| Emscripten | 4.0.7 (the version Qt 6.11.1 pins) |
| Transport | `QRemoteObjectNode::addClientSideConnection` over a `QIODevice` wrapping a `QWebSocket`, the pattern from Qt's own QtRemoteObjects WebSockets example |
| Host | native edge process, `QRemoteObjectHost` with `AllowExternalRegistration`, `addHostSideConnection` per accepted socket |
| Failing engine | Firefox, Playwright build `firefox-1532` |
| Failing host | GitHub hosted Ubuntu runner (`ubuntu-22.04` at the time of the diagnosis; the job label is `ubuntu-24.04` today) |
| Passing engines on the same runner | Chromium, WebKit |
| Passing hosts for the same engine | macOS runner; Arch Linux developer machine |

Qt documents QtRemoteObjects over QtWebSockets as not officially supported on WebAssembly.
This report is filed anyway, because the mechanism found is not specific to that transport:
it is about a queued signal being the sole completion path for an asynchronous call.

## What happens

The client acquires a replica, waits for it to initialise, and calls one returning slot,
`echo(QString)`. Expected: `QRemoteObjectPendingCallWatcher::finished` fires and the caller
reads `returnValue()`. Observed, in the failing configuration only:

```
FAIL connected=true prop=true signal=true reply=false model=true counters=[8..45]
```

`prop`, `signal` and `model` are edge-to-client pushes and they keep flowing for the entire
45 second window. `reply` is the one client-to-edge round trip, and it never completes. The
client logs no error either: no error reply, no socket error, nothing. The call simply stays
pending forever.

## Evidence, in the order it was gathered

Each step was written down with a decision rule before the run that answered it, so the
conclusion could not drift to fit the result.

1. **Not a code defect and not a Firefox version regression.** The identical bundle, the
   identical Playwright Firefox build (`firefox-1532`), on a developer machine: all cases
   pass, `reply=true`, in about two seconds. A stock Firefox 152 (newer than the Playwright
   build) driven over WebDriver BiDi against the same bundle and the same native edge: passes
   over both `ws` and `wss`. Chromium and WebKit pass on the failing runner itself. Firefox
   passes on the macOS runner.

2. **Not a one-shot loss.** A 2 second retry timer was added that re-issues the call while no
   reply has been seen. It fired roughly 25 and 45 times across the timeout in two jobs, and
   not one reply ever resolved. So the path is persistently dead, not racing.

3. **The uplink works.** The edge was instrumented to log each invocation. In the failing
   window the edge prints `M0 EDGE echo invoked message=m0-ping`, so the `InvokePacket`
   reaches the host, the slot runs, and it returns.

4. **The reply frame arrives at the client.** The `QIODevice` wrapper was instrumented to log
   frame sizes on both sides. In the failing window the edge writes a 69 byte reply frame and
   the client reads a 69 byte frame, 22 times over the window. QtRO's read loop is draining
   the socket the whole time, which the continuing property pushes independently confirm.

5. **The reply correlates correctly.** A serial-id trace was added to both sides. In the
   failing window every reply acknowledges the serial of its invocation: `1 -> 1`, `2 -> 2`,
   through `8 -> 8` (serial 0 lines are heartbeats). So `notifyAboutReply` is reached with a
   serial that is in `m_pendingCalls`, `take()` succeeds, and the call object is the right one.
   An earlier reading of this file said the serial did not match; that was inferred before the
   serials were visible and it is retracted.

6. **What is left is the queued hop.** `QConnectedReplicaImplementation::notifyAboutReply`
   (`qtremoteobjects/src/remoteobjects/qremoteobjectreplica.cpp:405-424`) sets
   `call.d->error = NoError` and `call.d->returnValue` under the call's mutex, then calls
   `watcherHelper->emitSignals()`. The watcher's `finished` reaches the caller through
   `QRemoteObjectPendingCallWatcherHelper::add`
   (`qtremoteobjects/src/remoteobjects/qremoteobjectpendingcall.cpp:30-35`), which connects with
   an explicit `Qt::QueuedConnection`; the same file posts a second queued emission at line 178
   for a call that had already finished when the watcher was constructed. A queued connection
   posts a `QEvent::MetaCall` and needs the event loop to deliver it. In this environment that
   delivery does not happen for that object, so the caller is never told about a reply that is
   already sitting in the call, fully decoded.

7. **Timers are unaffected, which is consistent.** The QtRO heartbeat and every `QTimer` keep
   firing throughout, because Emscripten drives timers through `setTimeout`, a different path
   from the posted-event queue. That is why the connection looks healthy while the completion
   never arrives.

8. **A teardown abort, probably a consequence.** With `-sASSERTIONS=1` the failing cases end
   with a bare `Aborted(native code called abort())` about 2.5 ms after the failure line, at
   the case boundary, once per Firefox case. It names no assert and no exception, and it fires
   after the failure rather than before it, so it reads as teardown of a page holding an
   unresolved pending call, not as the cause.

## Ruled out

- **The call being made before the replica is initialised.** The call is issued from the
  replica's own initialisation handler and guards on validity.
- **A null `d` pointer from a missed `take()`.** The default `QRemoteObjectPendingCall`
  constructor allocates `d` (`qremoteobjectpendingcall.cpp`), so a missed take is harmless and
  cannot produce this.
- **Correlation or framing.** See evidence 4 and 5: the frame arrives and the serial matches.
- **The driver.** See evidence 1: the same Playwright build passes elsewhere, and a stock
  Firefox driven over BiDi with no Playwright involved also passes. The runner environment is
  the variable, not the automation library.
- **A Firefox version regression.** A newer Firefox passes on a developer machine; the failing
  build passes there too.

## How to reproduce

The spike that produces this is small and self-contained, and it is kept in the SynQt
repository as a regression guard:

```sh
# builds the native edge, the WASM client, serves it, and drives every installed engine
tests/m0-transport/verify/run-m0.sh
```

It passes on every machine we have. Reproducing the failure needs the GitHub hosted Ubuntu
runner: the workflow is `.github/workflows/browser-matrix.yml`, and the failing cases were
`firefox-ws`, `firefox-wss` and `firefox-reconnect` in the `webkit-linux` job, which runs the
full matrix.

Note that the failure is currently masked by the workaround below. To see it again, take the
poll fallback out of `M0Controller` and of `src/consumer/promise.cpp`.

## Workaround shipped, and one that failed

**Shipped.** Resolve the reply from the call's own state instead of waiting for the queued
signal: keep the watcher, and add a `QTimer` (50 ms) under `#ifdef Q_OS_WASM` that checks
`isFinished()` and settles from `returnValue()`. Both paths are guarded so whichever fires
first wins. In SynQt this lives in `src/consumer/promise.cpp`, which is the single place the
framework resolves a QtRO reply, so every application call is covered rather than only the
test. On engines where the watcher works, the timer is a no-op.

**Failed, and worth recording.** The first attempt also added a 16 ms
`QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall)` nudge to force the real
watcher to fire. It broke reconnect on Firefox (the disconnect was seen, the reconnect never
completed), while Chromium survived it: force-draining every posted meta-call on a timer
reorders Qt's event delivery. A global pump is too invasive. Resolve the specific call from
its own state instead.

## What would help from Qt

1. **Do not make a queued signal the only completion path.** A caller that holds a
   `QRemoteObjectPendingCall` cannot ask "are you finished" without either blocking
   (`waitForFinished`) or attaching a watcher whose delivery depends on the posted-event
   queue. When that queue is starved, a fully decoded reply is unreachable. A direct
   completion callback, or a documented non-blocking way to observe completion, would make
   this class of environment failure survivable.
2. **`QRemoteObjectPendingCall::d` is protected**, so an application cannot even null-check it
   from outside while diagnosing. A read-only accessor for the call's state would have saved
   most of the probing above.
3. **An explanation of the pump starvation** would be the real fix: why an Emscripten
   `QEvent::MetaCall` posted for one object is not drained under Firefox on that runner while
   timers keep firing, and while Chromium and WebKit on the same runner drain it. We could not
   reproduce it off that host, so we could not chase it further.

## Status

Masked, not fixed. SynQt ships the poll fallback and its CI is green, so the condition is
invisible unless it is looked for. The M0 spike logs `M0 slot reply=<value> (via poll fallback)`
whenever the fallback is what resolved the call, which is the marker to grep for in a run log
to tell whether the underlying behaviour is still present.
