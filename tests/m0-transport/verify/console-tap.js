// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Keep the page's own console history, so a driver that cannot read the console can still read
// the evidence.
//
// The M0 harness asserts on single-line "M0 ..." sentinels the client prints, and Playwright
// delivers those through its console event. Real Safari is driven through safaridriver, which
// speaks W3C WebDriver, and WebDriver standardises no logging endpoint at all: Apple implements
// none, so there is no protocol-level way to ask Safari what the page logged. Without this the
// only Safari harness possible would be one that asserts on rendered pixels, which would prove
// far less about the four QtRO paths than the sentinels do.
//
// So the page records for itself, and the driver reads the array back with an execute/sync call.
// This is a same-origin script rather than an inline one on purpose: the multi-threaded proof
// serves a strict `script-src 'self' 'wasm-unsafe-eval'`, which an inline tap would violate,
// and the point of that proof is to run under the policy the edge really emits.
(function () {
    "use strict";

    // Bounded: the frame-size instrument prints 1-2 lines a second and a reconnect case runs for
    // a minute or more, so an unbounded array would grow without limit in the one process whose
    // memory pressure the proof is trying to measure.
    var LIMIT = 20000;
    var logs = [];

    window.__synqtLogs = logs;

    function record(text) {
        logs.push(text);
        if (logs.length > LIMIT) {
            logs.splice(0, logs.length - LIMIT);
        }
    }

    // Every level, because Qt routes qWarning and qCritical to console.warn and console.error;
    // capturing only console.log would silently drop "M0 socket error=..." and leave a failing
    // case with no account of why it failed.
    ["log", "info", "warn", "error", "debug"].forEach(function (level) {
        var original = console[level];
        console[level] = function () {
            try {
                record(Array.prototype.map.call(arguments, String).join(" "));
            } catch (ignored) {
                // Never let the tap break the page it is observing.
            }
            if (original) {
                original.apply(console, arguments);
            }
        };
    });

    window.addEventListener("error", function (event) {
        record("PAGEERROR " + (event.message || String(event.error)));
    });
    window.addEventListener("unhandledrejection", function (event) {
        record("PAGEERROR unhandled rejection: " + String(event.reason));
    });

    // The CSP proof reads these back the same way the sentinels are read. Recorded rather than
    // counted so a violation names the directive that rejected it.
    document.addEventListener("securitypolicyviolation", function (event) {
        record("CSPVIOLATION " + event.effectiveDirective + " blocked " + event.blockedURI);
    });
}());
