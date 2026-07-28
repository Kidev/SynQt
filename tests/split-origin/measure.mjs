// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What a split-origin session cookie can rely on, measured in real browsers.
//
// `project.origin_model: split_origin` puts the client on one site and the edge on another,
// so the session cookie is a third-party cookie. That is a browser policy question, not a Qt
// question, and it is the reason split-origin is a hand-written setting rather than something
// `synqt new` offers. This rig measures the policy so the documentation quotes data.
//
// Two sites, one loopback: synqtcdn.test delivers the client, synqtedge.test is the edge.
// They must be separate registrable domains, not two names under one domain, or the browser
// treats them as one site and no third-party rule applies at all. The `lax_control` variant
// exists to prove that: a SameSite=Lax cookie must fail every cross-site read, and if it ever
// passes, the rig has stopped measuring anything and every other result is void.
//
// The server here stands in for the edge, so the results describe what any edge can rely on.

import { createServer } from "node:https";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { lookup } from "node:dns/promises";
import { chromium, firefox, webkit }
    from "../m0-transport/verify/node_modules/playwright/index.mjs";

const PORT = Number(process.env.SPLIT_ORIGIN_PORT || 8443);
const CDN = `https://synqtcdn.test:${PORT}`;
const EDGE = `https://synqtedge.test:${PORT}`;
const COOKIE = "synqt_session";
const CERT_DIR = process.env.SPLIT_ORIGIN_CERTS || ".";

// `unpartitioned` is what the edge emits today. `partitioned` is CHIPS, the spelling browsers
// keep accepting as they withdraw the unpartitioned kind. `lax_control` is the rig's own
// self-test and is never a candidate.
const VARIANTS = {
    unpartitioned: `${COOKIE}=tok-%s; HttpOnly; Path=/; SameSite=None; Secure`,
    partitioned: `${COOKIE}=tok-%s; HttpOnly; Path=/; SameSite=None; Secure; Partitioned`,
    lax_control: `${COOKIE}=tok-%s; HttpOnly; Path=/; SameSite=Lax; Secure`,
};

let variant = "unpartitioned";
let issued = 0;

function carriesCookie(request) {
    return String(request.headers.cookie || "").includes(`${COOKIE}=`);
}

const PAGE = `<!doctype html><meta charset="utf-8"><title>cdn</title><body><script>
window.results = {};
async function call(path) {
    const response = await fetch("${EDGE}" + path, { credentials: "include", cache: "no-store" });
    return (await response.json()).carried;
}
window.bootstrap = () => call("/session");
window.whoami = () => call("/whoami");
window.sync = () => new Promise((resolve) => {
    // The upgrade is the request that decides the app: a session the wss handshake cannot
    // carry is a session the edge never sees, however well the page loaded.
    const socket = new WebSocket("${EDGE.replace("https", "wss")}/sync");
    socket.onmessage = (event) => resolve(event.data === "cookie");
    socket.onerror = () => resolve(false);
    setTimeout(() => resolve(false), 4000);
});
</script></body>`;

function setCookieHeader() {
    return VARIANTS[variant].replace("%s", String(++issued));
}

const server = createServer(
    {
        cert: readFileSync(`${CERT_DIR}/cert.pem`),
        key: readFileSync(`${CERT_DIR}/key.pem`),
    },
    (request, response) => {
        const host = String(request.headers.host || "").split(":")[0];
        const cors = {
            "Access-Control-Allow-Origin": request.headers.origin || CDN,
            "Access-Control-Allow-Credentials": "true",
            Vary: "Origin",
        };
        if (host === "synqtcdn.test") {
            response.writeHead(200, { "Content-Type": "text/html" });
            response.end(PAGE);
            return;
        }
        if (request.url.startsWith("/session")) {
            // The credential bootstrap: a credentialed subresource request made from the page
            // that will later read the cookie back.
            const carried = carriesCookie(request);
            response.writeHead(200, { ...cors, "Content-Type": "application/json",
                                      "Set-Cookie": setCookieHeader() });
            response.end(JSON.stringify({ carried }));
            return;
        }
        if (request.url.startsWith("/login")) {
            // The OAuth callback: a top-level navigation that lands on the edge, sets the
            // session, and sends the browser back to where the client is served from. The
            // top-level site here is the edge, which is what decides a partitioned cookie's
            // partition key, and therefore what breaks.
            response.writeHead(302, { Location: `${CDN}/`, "Set-Cookie": setCookieHeader() });
            response.end();
            return;
        }
        response.writeHead(200, { ...cors, "Content-Type": "application/json" });
        response.end(JSON.stringify({ carried: carriesCookie(request) }));
    });

server.on("upgrade", (request, socket) => {
    const carried = carriesCookie(request);
    const accept = createHash("sha1")
        .update(request.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
        .digest("base64");
    socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        + `Connection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
    const payload = Buffer.from(carried ? "cookie" : "none");
    socket.write(Buffer.concat([Buffer.from([0x81, payload.length]), payload]));
});

// Each engine reaches the two test sites its own way. WebKit has neither a host resolver
// flag nor a DNS pref, so it is measurable only when the names already resolve, which is
// what `hostsFileMapsTheSites` checks; CI adds them to /etc/hosts for exactly that reason.
// Skipping is reported, never silent: an unmeasured engine that looks like a passing one is
// how a documentation table starts lying.
function launchOptions(engine) {
    if (engine === chromium) {
        return {
            headless: true,
            args: [
                "--ignore-certificate-errors",
                "--host-resolver-rules=MAP synqtcdn.test 127.0.0.1, MAP synqtedge.test 127.0.0.1",
            ],
        };
    }
    if (engine === firefox) {
        return {
            headless: true,
            firefoxUserPrefs: { "network.dns.localDomains": "synqtcdn.test,synqtedge.test" },
        };
    }
    return { headless: true };
}

async function hostsFileMapsTheSites() {
    try {
        await lookup("synqtcdn.test");
        await lookup("synqtedge.test");
        return true;
    } catch {
        return false;
    }
}

// Restricting third-party cookies is the end state the unpartitioned cookie is being
// withdrawn into. The `--test-third-party-cookie-phaseout` command line flag is accepted and
// does nothing here (measured: results identical with and without it), so the restriction is
// driven through CDP, which reports whether it took the command. Network.enable first, or
// setCookieControls is accepted and silently ignored the same way.
async function restrictThirdPartyCookies(context, page) {
    const cdp = await context.newCDPSession(page);
    await cdp.send("Network.enable");
    await cdp.send("Network.setCookieControls", {
        enableThirdPartyCookieRestriction: true,
        disableThirdPartyCookieMetadata: true,
        disableThirdPartyCookieHeuristics: true,
    });
}

async function measure(engine, restricted) {
    const results = {};
    for (const flavour of Object.keys(VARIANTS)) {
        variant = flavour;
        const browser = await engine.launch(launchOptions(engine));
        const context = await browser.newContext({ ignoreHTTPSErrors: true });
        const page = await context.newPage();
        if (restricted) {
            await restrictThirdPartyCookies(context, page);
        }

        // 1. Bootstrap: the cookie is set from the page that will read it back.
        await page.goto(`${CDN}/`, { waitUntil: "load" });
        await page.evaluate("window.bootstrap()");
        const bootstrapRead = await page.evaluate("window.whoami()");
        const upgrade = await page.evaluate("window.sync()");

        // 2. Login: the cookie is set during a top-level navigation to the edge, and must
        //    then be readable from the client page it returns to.
        await context.clearCookies();
        await page.goto(`${EDGE}/login`, { waitUntil: "load" });
        const returnedToClient = page.url().startsWith(CDN);
        const afterLoginRead = await page.evaluate("window.whoami()");

        const stored = await context.cookies();
        results[flavour] = {
            bootstrapRead,
            upgrade,
            returnedToClient,
            afterLoginRead,
            partitionKey: stored.length ? (stored[0].partitionKey ?? null) : "no-cookie-stored",
        };
        await browser.close();
    }
    return results;
}

await new Promise((resolve) => server.listen(PORT, "127.0.0.1", resolve));
const resolvable = await hostsFileMapsTheSites();
const report = {};
for (const [name, engine, restricted] of [
    ["chromium", chromium, false],
    ["chromium-3pc-restricted", chromium, true],
    ["firefox", firefox, false],
    ["webkit", webkit, false],
]) {
    if (engine === webkit && !resolvable) {
        report[name] = {
            skipped: "needs synqtcdn.test and synqtedge.test in /etc/hosts; WebKit has no "
                + "host resolver override",
        };
        continue;
    }
    try {
        report[name] = await measure(engine, restricted);
    } catch (error) {
        report[name] = { error: String(error).split("\n")[0] };
    }
}
server.close();
console.log(JSON.stringify(report, null, 2));
process.exit(0);
