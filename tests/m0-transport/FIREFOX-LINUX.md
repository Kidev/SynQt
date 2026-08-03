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

Why an undelivered meta-call is permanent rather than late is a property of
`QEventDispatcherWasm`, and it is the same in every engine. `QCoreApplication::postEvent()`
reaches `wakeUp()`, which on the main thread arms a zero-delay `QWasmTimer` from inside an
`emscripten_async_call()` that is itself a zero-delay browser callback. That chain of two
callbacks is the only thing that ever calls `QCoreApplication::sendPostedEvents()`, and
`wakeUp()` runs when an event is posted, not again while it waits. `onTimer()`, the other
callback that keeps arriving, sends timer events and only timer events. So dropping either
hop once loses the event for good, in an application that goes on looking healthy. That is
reproducible anywhere, it has a fix, and both are in
[qt-patches/](qt-patches/README.md). What remains specific to Firefox on that runner is what
dropped the callback in the first place.

All of that describes the dispatcher's non-asyncify shape, which is the default and the only
one that works in every browser. Linking the client with `-sASYNCIFY` selects the other shape,
where any browser event at all drives a posted-event sweep, and that is immune. It needs no
change to Qt and it is measured below.

The code above is `qtbase` 6.11.1, and it is unchanged on `dev` as of `v6.12.0-beta1-1287`
(`59b2fd0a4fa`): `diff` of `qeventdispatcher_wasm.cpp` between the two shows only an unrelated
removal of the `qtLoaded` startup-task helpers. `wakeUp()` still guards on
`m_wakeupTimer->hasTimeout()`, `QWasmTimer::hasTimeout()` is still `m_timerId > 0` cleared
only by the callback that runs, and `onTimer()` still sends timer events only. So this is not
a fixed-in-a-later-release problem.

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

9. **The queued hop, reproduced off the runner.** Everything above says what is not
   delivered. This says why nothing recovers, and it needs no special environment.
   `QEventDispatcherWasm::wakeUp()`
   (`qtbase/src/corelib/kernel/qeventdispatcher_wasm.cpp:363-382`) arms a zero-delay
   `QWasmTimer`, from inside `qwasmglobal::runOnMainThreadAsync()`, which is an
   `emscripten_async_call()` with a zero timeout: two browser callbacks in a chain, and the
   only path to `QCoreApplication::sendPostedEvents()` on the main thread. `onTimer()`
   (line 468) calls `sendTimerEvents()` and nothing else, and
   `QTimerInfoList::activateTimers()` delivers with `sendEvent()`, so timers never touch the
   posted queue and cannot stand in for it. Since `wakeUp()` is called when an event is
   posted and not again while it waits, losing either callback loses the event permanently.
   [verify/verify-pump.mjs](verify/verify-pump.mjs) drops exactly the timeout `wakeUp()` arms
   and leaves the native timer alone; on a stock kit both Chromium and Firefox then show this
   report's signature exactly, locally, every run.

## Why Firefox, and why that one runner

The natural reading of "only Firefox, only that runner, and all three of its cases, run after
run" is that something in that combination behaves differently, systematically. It does not
have to, and the measurement that settles it is this: **one lost callback is permanent.**

`node verify-pump.mjs stall once` drops exactly one wakeup arm and then gets out of the way.
Both Chromium and Firefox then spend the rest of the session wedged, with property pushes
still arriving and the watcher never firing again. The reason is in `QWasmTimer::setTimeout`:
the arm that was dropped still returned a live timer id, `hasTimeout()` reads true from then
on, and `wakeUp()`'s `if (!hasTimeout()) setTimeout(0ms)` never arms another. Nothing clears
it, because the only thing that clears it is the callback that never came.

So the trigger does not need to be systematic, or even common. It needs to happen once, in
one session, and the failure that follows is total and looks perfectly reproducible. That is
the shape of the original report.

Which leaves susceptibility rather than mechanism as the question, and on that the honest
answer is that we measured, ruled things out, and did not catch it in the act:

- **Not requestAnimationFrame.** `emscripten_async_call(fn, arg, 0)` takes the
  `millis >= 0` branch and uses `setTimeout` (emsdk 4.0.7, `src/lib/libeventloop.js`), so both
  hops of the chain are ordinary zero-delay timeouts.
- **Not CPU starvation, as far as we can model it.** Pinned to two saturated cores, both
  engines still delivered every wakeup: Chromium zero-delay p95 4.1 ms, Firefox 4 ms, and the
  reply resolved in both.
- **Not timer deprioritisation under load.** With the main thread kept busy by a
  `MessageChannel` loop spinning 8 ms a turn, both engines delivered wakeups at exactly one
  spin quantum, p50 8 ms.
- **Firefox does schedule timers more loosely than Chromium**, which is the one measured
  difference and the reason it is the likeliest of the three to lose one: idle, its zero-delay
  arms ran at p95 10 ms against Chromium's 0.7 ms, and its interval timers at p99 24 ms
  against Chromium's 0.3 ms.

A hosted runner is the slowest, most contended, GPU-less machine in the matrix, and Firefox
is the engine with the loosest timer scheduling on it. That is a plausible place for a
one-in-a-session event, and it is as far as the evidence goes. The patch below is what makes
the question stop mattering.

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

It passes on every machine we have. Reproducing the *environment* needs the GitHub hosted
Ubuntu runner: the workflow is `.github/workflows/browser-matrix.yml`, and the failing cases
were `firefox-ws`, `firefox-wss` and `firefox-reconnect` in the `webkit-linux` job, which
runs the full matrix. Note that the failure is masked there by the workaround below; to see
it again, take the poll fallback out of `M0Controller` and of `src/consumer/promise.cpp`.

Reproducing the *mechanism* needs nothing but a browser, and this is the one to run:

```sh
cd tests/m0-transport/verify
node verify-pump.mjs stall      # stock Qt: the watcher must never fire
node verify-pump.mjs recover    # patched Qt: the watcher must fire anyway
```

It starves the one browser timeout `wakeUp()` arms, leaving the native Qt timer, the socket
and everything else alone, and then reports the same booleans this document opens with. It
also distinguishes the two ways the echo can resolve, which is the measurement that matters:
the poll fallback resolving it proves the reply arrived and decoded, and the watcher not
resolving it proves the caller was never told.

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

## The fix, and what is still open

**Give posted events a second way to be delivered.** The patch in
[qt-patches/](qt-patches/README.md) makes `QEventDispatcherWasm::onTimer()` send posted
events as well as timer events. It is four lines. It does not make the browser stop dropping
callbacks; it stops one dropped callback from being fatal, turning an event that is never
delivered into one delivered at most a timer interval late. Measured against the
reproduction above, on Qt 6.11.1 with Emscripten 4.0.7: on a stock kit the watcher never
fires in either Chromium or Firefox, on a patched kit it fires in both, and the full M0 gate
stays green, reconnect included.

**Doing the same thing from application code does not work**, which is worth stating plainly
because it is the obvious thing to try instead of patching Qt, and it was tried twice. The
first attempt, above, pumped `sendPostedEvents(nullptr, QEvent::MetaCall)` every 16 ms and
broke reconnect on Firefox; the natural theory was that the event-type filter was at fault,
since draining one type out of a queue holding several reorders them. It is not the filter.
Build the M0 client with `-DM0_POSTED_EVENT_PUMP=ON` (`client/main.cpp`) and it sweeps the
queue with the plain, unfiltered `QCoreApplication::sendPostedEvents()`, the same call the
dispatcher makes, on a 50 ms `QTimer`. It fixes the stall in both engines. It also failed
`firefox-reconnect` in one run out of four, with the same `disconnect=true reconnect=false`
signature as the first attempt.

So the difference is not what is drained but where from. Sweeping the queue from inside a
`QTimer` handler runs it nested inside the dispatcher's own `sendAllEvents()` pass, which is
not a place Qt expects a sweep. `onTimer()` runs before that pass begins, in the same order
`sendAllEvents()` uses. That is why the fix belongs in the dispatcher, and why the option
above is left off by default: it is kept as the measurement, not as a recommendation.

## What a wedged pump actually costs an application

Measured or read off the source, not guessed. What keeps working is most of the client:
QtRemoteObjects property, signal and model updates (activated directly from the socket read
callback), every `QTimer` (`QTimerInfoList::activateTimers()` delivers with `sendEvent()`),
the socket including reconnect, and `QNetworkReply::finished` (on WebAssembly
`qnetworkreplywasmimpl.cpp` emits it directly from the fetch callback, so login and the
session fetch still complete). Nothing errors, nothing logs, and the connection stays up.

What stops:

- **Returning slots never resolve.** `QRemoteObjectPendingCallWatcher::finished` is QtRO's
  only non-blocking completion path and it is queued. This is the original report.
- **The browser's back and forward buttons**, for any client that hands `popstate` to Qt
  with a queued invocation.
- **`Qt.callLater()` in QML**, which schedules its tick with a `Qt::QueuedConnection`
  (`qqmldelayedcallqueue.cpp`).
- **Every QML `Timer`, animation, `Behavior` and transition that has not already started.**
  `QUnifiedTimer` registers an animation with
  `QMetaObject::invokeMethod(inst, "startTimers", Qt::QueuedConnection)`
  (`qabstractanimation.cpp`), so starting one needs the queue. This is the widest
  consequence and the least obvious: it was found by writing the regression test, when a
  QML `Timer` that should have fired 800 ms after connecting never fired at all.
- **Every `deleteLater()`**, which posts a `QEvent::DeferredDelete`. The objects are not
  merely unreclaimed, they stay live and connected.

SynQt's client runtime does not rely on any of these: replies settle from the call's own
state, `popstate` is delivered directly, and deferred deletion goes through
`SynQt::deleteSoon`, which uses a zero-delay timer. The `-starved` cases in
`tests/m6-client/verify/verify.mjs` hold that. An application's own queued connections and
QML animations are still exposed, which is what `build.client_asyncify` is for.

Two things would still help, and one question is still open.

1. **Do not make a queued signal the only completion path.** A caller that holds a
   `QRemoteObjectPendingCall` cannot ask "are you finished" without either blocking
   (`waitForFinished`) or attaching a watcher whose delivery depends on the posted-event
   queue. The dispatcher patch makes that queue recoverable, but a direct completion
   callback would mean QtRO did not depend on it at all.
2. **`QRemoteObjectPendingCall::d` is protected**, so an application cannot even null-check it
   from outside while diagnosing. A read-only accessor for the call's state would have saved
   most of the probing above.
3. **Why the callback was dropped** is still unanswered: what makes Firefox on that runner
   lose a zero-delay timeout that Chromium and WebKit on the same runner, and Firefox
   everywhere else, deliver. We could not reproduce that off the host. The patch means we no
   longer have to.

## Asyncify already has the second path, and needs no change to Qt

`QEventDispatcherWasm` has two shapes, and the one described above is only the non-asyncify
one. Which shape is used is decided by `qstdweb::haveAsyncify()`, which is a runtime probe of
the Emscripten runtime (`EM_JS(bool, jsHaveAsyncify, (), { return typeof Asyncify !==
"undefined"; })`, `qstdweb.cpp:97`) and not a Qt build option, so an application can select
the other shape by adding `-sASYNCIFY` to its own link line. The prebuilt Qt kit does not have
to be rebuilt or patched.

The two shapes differ in exactly the way this defect is about. Without asyncify the main
thread cannot block, so `QCoreApplication::exec()` hands control back to the browser
(`handleNonAsyncifyErrorCases()`) and `processEvents()` afterwards runs only when the wakeup
timer fires. With asyncify the main thread suspends inside `processEvents()`
(`processEventsWait()` -> `asyncifyWait(std::nullopt)` -> `QWasmSuspendResumeControl::suspend()`),
and every handler registered with that control resumes it: every DOM event, every socket
callback, every Qt timer. On resume, `processEvents()` calls `sendAllEvents()`, whose first
step is `sendPostedEvents()`. So the posted queue is swept by anything the page does, and no
single dropped callback can take that away.

Measured on the same stock 6.11.1 kit, same edge, same shim, Chromium 149 and Firefox 151:

| client link | `stall` | `recover` |
|---|---|---|
| as shipped | pass, both engines | (not run) |
| `-sASYNCIFY` | fail, both engines | pass, both engines |

`stall` failing is the result being looked for: the watcher fired even with every wakeup
starved. It fired on the first echo, with `pollReply=false`, so the poll fallback never had to
run at all. The full M0 gate (`node verify.mjs`) also passes on the asyncify build in both
engines over `ws` and `wss`, reconnect included, which is where the application-side pump
failed.

Build it with `-DM0_ASYNCIFY=ON` and drive it with `M0_CLIENT_DIR` pointing at that build.

The cost is the bundle. This spike, `-O2` as shipped against `-Os` plus asyncify (Emscripten's
own recommended pairing), grew from 25.8 MB to 39.2 MB of wasm, and from 8.7 MB to 11.6 MB
gzipped: about a third more over the wire. Asyncify also instruments every function that can
be on a suspend stack, which costs run time; that was not measured here. JSPI
(`-sJSPI`, what Qt calls asyncify 2) avoids both costs, but only Chromium ships it, so it
cannot be the portable answer. Asyncify also widens what the client may do, since
`QEventLoop::exec()` on the main thread starts working instead of calling `qFatal()`.

## Status

Fixed in a local Qt, masked in the shipped one. The patch is not upstream and CI builds
against a stock Qt, so SynQt keeps the poll fallback in `src/consumer/promise.cpp` until the
fix lands in a Qt release. The M0 spike logs `M0 slot reply=<value> (via poll fallback)`
whenever the fallback is what resolved the call, which is the marker to grep for in a run log
to tell whether the underlying behaviour is still present.
