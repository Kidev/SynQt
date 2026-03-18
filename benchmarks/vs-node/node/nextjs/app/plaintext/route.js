// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// `force-dynamic` on every one of these six, including the two that answer a constant.
//
// Without it Next.js renders a Route Handler with no request-dependent input at build time
// and serves the answer from disk, which is a real and good thing for a website and makes
// this table meaningless: the /plaintext and /json columns would be measuring a static file
// server against two frameworks doing work. The other four read a database and would not be
// cached anyway; it is written on all six so the six are one test.
export const dynamic = "force-dynamic";

export function GET() {
    return new Response("Hello, World!", {
        headers: {"content-type": "text/plain"},
    });
}
