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
// the wakeup timer is the one that is only ever armed with a zero delay, while the native
// timer carries the interval of the shortest live QTimer. So the shim watches every arm,
// remembers per handler whether it has ever seen a nonzero delay, and starves only handlers
// that have not. Emscripten's own zero-delay timeouts, DOM events, and the WebSocket's
// message callback are all untouched.
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
    let sawIntervalTimer = false;

    function isQtHandler(fn) {
        let profile = profiles.get(fn);
        if (!profile) {
            profile = { qt: String(fn).indexOf("pendingEvents.push") !== -1, nonzero: 0, zero: 0 };
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
            sawIntervalTimer = true;
        }
        if (observe) {
            console.log("M0PUMP arm handler #" + profile.id + " delay=" + (delay || 0) +
                        " zero=" + profile.zero + " nonzero=" + profile.nonzero);
        }
        // Only ever armed with zero: this is the wakeup timer, not the native Qt timer. The
        // native timer is armed with the shortest live QTimer's remaining wait, which is
        // occasionally 0 for an overdue timer, so its zero arms are not on their own a tell;
        // what settles it is that some handler has already been armed with a real interval,
        // which identifies the native timer as a different function from this one. Starving
        // the native timer by mistake would stop the clock rather than the pump, and the case
        // would report "never reached a state worth measuring" rather than pass for the wrong
        // reason.
        if (starving && isZeroDelay && profile.nonzero === 0 && sawIntervalTimer
                && !(once && dropped > 0)) {
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
