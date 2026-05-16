// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import {renderFortunes} from "../../../techempower.mjs";
import {store} from "../store.js";

export const dynamic = "force-dynamic";

export function GET() {
    return new Response(renderFortunes(store().selectFortunes.all()), {
        headers: {"content-type": "text/html; charset=utf-8"},
    });
}
