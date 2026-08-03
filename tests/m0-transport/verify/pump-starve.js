// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Starves Qt for WebAssembly's posted-event wakeup, and nothing else.
//
// QCoreApplication::postEvent() calls QEventDispatcherWasm::wakeUp(), which on the main
// thread arms a zero-delay browser timeout (QWasmTimer::setTimeout(0ms) ->
// window.setTimeout(handler, 0)) whose callback is the only thing that ever calls
// QCoreApplication::sendPostedEvents(). Qt's own timers ride a different QWasmTimer, armed
// with the QTimer's real interval, and QTimerInfoList::activateTimers() delivers them with
// sendEvent(), not postEvent(). So the two paths are independent, and starving one leaves
// the other running: exactly the state the Firefox-on-CI failure reports, where heartbeats
// and property pushes flow for the whole window while a queued QEvent::MetaCall never
// arrives.
//
// Telling the two QWasmTimers apart from JavaScript, given that they share one handler body
// and differ only in which C++ object owns them: a Qt suspend/resume handler is recognisable
// by its source (it pushes onto the control object's pendingEvents queue), and among those,
// the wakeup timer is the one that is never armed with a delay. QWasmTimer::setTimeout()
// passes the interval straight to window.setTimeout(), and the wakeup's interval is always
// 0ms (qeventdispatcher_wasm.cpp), while the native timer carries the interval of the
// shortest live QTimer.
//
// So this does not try to name the wakeup. Every Qt handler is treated as a candidate, and
// an arm with a real delay retires that handler for good: it was a QTimer, and starving it
// would stop the clock rather than the pump. What is left is starved on every zero-delay
// arm, whichever handler index it turns out to be. An earlier version of this file picked
// the first handler armed with a zero delay and starved only that one, which is a race:
// which of the two arms first depends on whether the application posts an event before it
// starts a timer, and on a slow machine it does not. That is exactly how the Firefox column
// failed on CI while passing on every workstation. Emscripten's own zero-delay timeouts, DOM
// events, and the WebSocket's message callback are all untouched.
//
// A starved arm still returns a real, live timer id, because QWasmTimer stores it and reads
// a nonzero id as "a wakeup is already pending", which is what makes the starvation stick
// instead of re-arming on the next postEvent().
(function () {
    const nativeSetTimeout = window.setTimeout.bind(window);
    const profiles = new Map();
    const mode = new URL(window.location.href).searchParams.get("starve");
    const observe = mode === "observe";
    // "once" drops a single wakeup and then gets out of the way, which is the question of
    // whether one dropped browser callback is a hiccup or a permanent wedge. It is permanent:
    // the arm that was dropped still returned a live timer id, so QWasmTimer::hasTimeout()
    // reads true forever after and wakeUp() never arms another. That is why this failure does
    // not need a systematic cause to look systematic.
    const once = mode === "once";
    let starving = false;
    let dropped = 0;

    function isQtHandler(fn) {
        let profile = profiles.get(fn);
        if (!profile) {
            profile = {
                qt: String(fn).indexOf("pendingEvents.push") !== -1,
                nonzero: 0,
                zero: 0,
                retired: false
            };
            profiles.set(fn, profile);
            if (profile.qt) {
                profile.id = profiles.size;
                console.log("M0PUMP saw Qt timer handler #" + profile.id);
            }
        }
        return profile;
    }

    window.setTimeout = function (fn, delay) {
        if (typeof fn !== "function") {
            return nativeSetTimeout.apply(null, arguments);
        }
        const profile = isQtHandler(fn);
        if (!profile.qt) {
            return nativeSetTimeout.apply(null, arguments);
        }
        const isZeroDelay = !delay || delay <= 0;
        if (isZeroDelay) {
            profile.zero += 1;
        } else {
            profile.nonzero += 1;
            if (!profile.retired) {
                // Armed with a real interval, so this one is a QTimer and not the wakeup.
                // Starving it would stop the clock instead of the pump, and the case would
                // then report a client that never got anywhere rather than one whose posted
                // events were lost.
                profile.retired = true;
                console.log("M0PUMP handler #" + profile.id + " was armed with delay=" +
                            delay + ", so it is a Qt timer, not the wakeup; leaving it alone");
            }
        }
        if (observe) {
            console.log("M0PUMP arm handler #" + profile.id + " delay=" + (delay || 0) +
                        " zero=" + profile.zero + " nonzero=" + profile.nonzero);
        }
        if (starving && isZeroDelay && !profile.retired && !(once && dropped > 0)) {
            dropped += 1;
            if (dropped <= 3 || dropped % 50 === 0) {
                console.log("M0PUMP dropped Qt wakeup timeout, total=" + dropped);
            }
            return nativeSetTimeout(function () {}, 0);
        }
        return nativeSetTimeout.apply(null, arguments);
    };

    window.__m0StarvePump = function () {
        starving = true;
        console.log("M0PUMP starving the Qt posted-event wakeup");
    };
    window.__m0PumpDropped = function () {
        return dropped;
    };

    if (mode === "load" || once) {
        window.__m0StarvePump();
    }
})();
