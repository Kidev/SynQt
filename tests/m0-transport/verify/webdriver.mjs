// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A minimal W3C WebDriver client, enough to drive real Safari.app through safaridriver.
//
// Written rather than pulled in: the whole surface this harness needs is four endpoints (new
// session, navigate, execute script, delete session), and the alternative is a dependency an
// order of magnitude larger than the code it replaces, in a verify directory whose only other
// dependency is the browser automation library that cannot do this job. Playwright drives
// WebKit, which is Safari's engine but not Safari: no Apple TLS stack, no Safari-specific
// networking, and the last mile docs/browser-proofs.md calls out as untested.
//
// safaridriver ships inside macOS (/usr/bin/safaridriver, a cryptex path on recent releases) and
// needs `safaridriver --enable` once per machine, which requires sudo. It refuses to run more
// than one session at a time, and it cannot run headless: Safari is a real window on a real
// display, so this only works in a logged-in GUI session.

import { spawn } from "node:child_process";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class WebDriverError extends Error {}

async function call(base, method, path, body) {
    const response = await fetch(`${base}${path}`, {
        method,
        headers: { "Content-Type": "application/json" },
        body: body === undefined ? undefined : JSON.stringify(body)
    });
    const text = await response.text();
    let payload = null;
    try {
        payload = text ? JSON.parse(text) : null;
    } catch {
        throw new WebDriverError(`${method} ${path}: non-JSON reply: ${text.slice(0, 200)}`);
    }
    // W3C puts both results and errors under "value"; an error is an object carrying `error`.
    const value = payload && "value" in payload ? payload.value : payload;
    if (!response.ok) {
        const message = (value && (value.message || value.error)) || response.statusText;
        throw new WebDriverError(`${method} ${path}: ${message}`);
    }
    return value;
}

/** Start safaridriver on `port` and wait for it to answer /status. */
export async function startSafariDriver(port) {
    const proc = spawn("safaridriver", ["-p", String(port)], {
        stdio: ["ignore", "pipe", "pipe"]
    });
    let stderr = "";
    proc.stderr.on("data", (chunk) => {
        stderr += String(chunk);
    });
    proc.on("error", (err) => {
        stderr += String(err.message);
    });

    const base = `http://127.0.0.1:${port}`;
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline) {
        if (proc.exitCode !== null) {
            throw new WebDriverError(
                `safaridriver exited (code ${proc.exitCode}). ` +
                    `Run \`sudo safaridriver --enable\` once per machine. ${stderr.trim()}`);
        }
        try {
            const status = await call(base, "GET", "/status");
            if (status && status.ready !== false) {
                return { proc, base };
            }
        } catch {
            // Not listening yet.
        }
        await sleep(250);
    }
    proc.kill("SIGKILL");
    throw new WebDriverError(
        `safaridriver did not become ready on port ${port}. ${stderr.trim()}`);
}

export class Session {
    constructor(base, id) {
        this.base = base;
        this.id = id;
    }

    static async create(base, capabilities = {}) {
        const value = await call(base, "POST", "/session", {
            capabilities: { alwaysMatch: { browserName: "safari", ...capabilities } }
        });
        // Some drivers answer {sessionId, capabilities}, others nest it; accept both.
        const id = value.sessionId || (value.value && value.value.sessionId);
        if (!id) {
            throw new WebDriverError("no sessionId in the new-session reply");
        }
        return new Session(base, id);
    }

    navigate(url) {
        return call(this.base, "POST", `/session/${this.id}/url`, { url });
    }

    /**
     * Run `script` in the page and return its value. The body is a function body, so it must
     * `return` what it wants back, exactly as WebDriver specifies.
     */
    execute(script, args = []) {
        return call(this.base, "POST", `/session/${this.id}/execute/sync`, { script, args });
    }

    async close() {
        try {
            await call(this.base, "DELETE", `/session/${this.id}`);
        } catch {
            // A session that already died is not a failure of the case that used it.
        }
    }
}

/** Read back what the page's console tap recorded (see console-tap.js). */
export async function pageLogs(session) {
    const logs = await session.execute("return window.__synqtLogs || [];");
    return Array.isArray(logs) ? logs : [];
}
