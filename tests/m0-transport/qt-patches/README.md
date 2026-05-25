<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# A patch to Qt, and how to put it on a kit

One patch lives here. It is against Qt itself and not against SynQt, and it fixes the
condition written up in [FIREFOX-LINUX.md](../FIREFOX-LINUX.md): a returning
QtRemoteObjects slot whose reply arrives and decodes but whose caller is never told,
because `QRemoteObjectPendingCallWatcher::finished` travels over a `Qt::QueuedConnection`
and nothing delivers the posted `QEvent::MetaCall` behind it.

[0001-wasm-send-posted-events-from-the-native-timer.patch](0001-wasm-send-posted-events-from-the-native-timer.patch)
changes `QEventDispatcherWasm::onTimer()` in `qtbase` so the native timer callback sends
posted events as well as timer events. That is four lines and one comment, and it buys a
second, independent way to deliver a posted event. Stock Qt has exactly one. `wakeUp()`
arms a zero-delay `QWasmTimer` from inside an `emscripten_async_call()` that is itself a
zero-delay callback, and `wakeUp()` runs when an event is posted and not again while it
waits. Drop either of those two browser callbacks and the event is lost permanently, and
not merely delayed, in an application that otherwise looks healthy. Timers keep firing
because `QTimerInfoList::activateTimers()` uses `sendEvent()`, and sockets keep reading
because their callbacks are DOM events.

## Putting it on an installed kit

A prebuilt Qt kit is static archives, so this does not need Qt rebuilt. `apply-to-kit.sh`
recompiles the one translation unit against the kit's own installed headers and swaps the
object into `libQt6Core.a`:

```sh
tests/m0-transport/qt-patches/apply-to-kit.sh apply     # patch the kit
tests/m0-transport/qt-patches/apply-to-kit.sh status    # is this kit patched?
tests/m0-transport/qt-patches/apply-to-kit.sh revert    # put Qt's own archive back
tests/m0-transport/qt-patches/apply-to-kit.sh verify    # rebuild and check, change nothing
```

It defaults to `/opt/Qt/6.12.0/wasm_singlethread`, with `/opt/Qt/6.12.0/gcc_64` for
`moc` and `/opt/Qt/6.12.0/Src/qtbase` for the source. Set `QT_WASM`, `QT_HOST` and
`QT_SRC` for anything else. Anything already built has to be relinked afterwards, which
for the spike's client means deleting `build/m0-client/m0-client.wasm` and building
again.

Two compile flags matter here. `-DQT_BUILDING_QT` puts the file's logging categories in
the `QtPrivateLogging` inline namespace. Without it, the object exports
differently-mangled symbols that nothing else in QtCore references, and the swap quietly
changes QtCore's link surface. `-fexceptions` matches the libc++ ABI tags on the shipped
object. The script trusts neither flag. It compares the rebuilt object's exported symbols
against the shipped one's and refuses the swap unless the two sets are identical. That
check is also what `verify` runs on its own.

## Proving it does something

[../verify/verify-pump.mjs](../verify/verify-pump.mjs) reproduces the failure on any machine
and in any engine, by starving exactly the browser timeout that `wakeUp()` arms and nothing
else. Run it with the expectation you are testing:

```sh
cd tests/m0-transport/verify
node verify-pump.mjs stall      # stock Qt: the watcher must never fire
node verify-pump.mjs recover    # patched Qt: the watcher must fire anyway
```

Run both directions. `stall` proves the reproduction is real and not a harness that
always passes, and it is the failing test the patch turns green. Measured here on
2026-08-03, Qt 6.11.1, Emscripten 4.0.7, Chromium 149 and Firefox 151:

| | `stall` | `recover` |
|---|---|---|
| stock kit | pass, both engines | fail, both engines |
| patched kit | fail, both engines | pass, both engines |

A third mode shows the shape of the failure and gates nothing:

```sh
node verify-pump.mjs stall once   # drop ONE wakeup, then leave the page alone
```

Both engines stay wedged for the rest of the session. The arm that was dropped still
returned a live timer id, so `QWasmTimer::hasTimeout()` reads true from then on and
`wakeUp()` never arms another. One lost callback, ever, is enough, which is why a failure
with no systematic cause can look completely systematic.

## The version that does not patch Qt, and why it is not the answer

The obvious alternative is to sweep the queue from application code, and the spike's
client can. Build it with `-DM0_POSTED_EVENT_PUMP=ON` and `client/main.cpp` runs a 50 ms
`QTimer` calling the plain, unfiltered `QCoreApplication::sendPostedEvents()`, which is the
same call the dispatcher makes. It fixes the stall in both engines, and it fails
`firefox-reconnect` about one run in four, with `disconnect=true reconnect=false`.

That is the same regression an earlier 16 ms
`sendPostedEvents(nullptr, QEvent::MetaCall)` attempt produced. The tempting explanation
was the event-type filter, since draining one type out of a queue holding several reorders
them against each other. It is not the filter, because the unfiltered version regresses
too. What differs is where the sweep runs from. A `QTimer` handler is itself running inside
the dispatcher's `sendAllEvents()` pass, so the sweep is nested inside another one.
`onTimer()` runs before that pass begins, in the order `sendAllEvents()` already uses. The
option is off by default and kept as a measurement, and it is not a recommendation.

The full transport gate (`node verify.mjs`) stays green on the patched kit, reconnect
included, which is the comparison that matters.

## The version that does not patch Qt and does work: asyncify

`QEventDispatcherWasm` already has a shape with a second delivery path.
`qstdweb::haveAsyncify()` decides whether it is used, and that is a runtime probe of the
Emscripten runtime and not a Qt build option, so an application selects it by adding
`-sASYNCIFY` to its own link line against the same prebuilt kit. Without asyncify the main
thread cannot block, `exec()` returns to the browser, and `processEvents()` runs only when
the wakeup timer fires. With asyncify the main thread suspends inside `processEvents()` and
any registered handler resumes it, after which `sendAllEvents()` sweeps the posted queue as
its first step. Any DOM event, socket callback or Qt timer is then enough.

```sh
qt-cmake -S tests/m0-transport -B build/m0-client-asyncify -DSYNQT_M0_ENTITY=client -DM0_ASYNCIFY=ON
cd tests/m0-transport/verify
M0_CLIENT_DIR=$PWD/../../../build/m0-client-asyncify node verify-pump.mjs stall    # must FAIL
M0_CLIENT_DIR=$PWD/../../../build/m0-client-asyncify node verify-pump.mjs recover  # must pass
```

Measured on a stock kit with Chromium 149 and Firefox 151, `stall` fails and `recover`
passes in both engines. The watcher fires on the first echo with `pollReply=false`, so the
poll fallback was never needed. The full `verify.mjs` gate also passes on that build,
reconnect included.

The cost is size. This spike went from 25.8 MB of wasm to 39.2 MB, and from 8.7 MB to
11.6 MB gzipped, comparing the shipped `-O2` build against `-Os` plus asyncify. Asyncify
also instruments every function that can sit on a suspend stack, which costs run time and
was not measured here. JSPI avoids both costs, and only Chromium ships it.

## Status

Not upstream, and not fixed in 6.12.0 either. That was first checked against
`v6.12.0-beta1-1287`, where `qeventdispatcher_wasm.cpp` was identical to 6.11.1 apart from
an unrelated startup-task removal, and then against the released source the project pins.
This patch applies to `/opt/Qt/6.12.0/Src/qtbase` with no offset and no fuzz, so the
context it patches is byte-identical and the two-hop chain is unchanged. This is a local
patch on a local kit, and CI builds against a stock Qt, so the SynQt workaround it would
replace (the `Q_OS_WASM` poll in `src/consumer/promise.cpp`) stays where it is until
either the fix ships in a Qt release or SynQt links its client with asyncify.
