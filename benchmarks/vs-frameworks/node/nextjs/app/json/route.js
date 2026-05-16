// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Dynamic for the reason plaintext/route.js gives.
export const dynamic = "force-dynamic";

export function GET() {
    return Response.json({message: "Hello, World!"});
}
