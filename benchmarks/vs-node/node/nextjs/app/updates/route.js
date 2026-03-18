// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import {clampedQueryCount} from "../../../techempower.mjs";
import {store} from "../store.js";

export const dynamic = "force-dynamic";

export function GET(request) {
    const count = clampedQueryCount(new URL(request.url).searchParams.get("queries"));
    return Response.json(store().runUpdates(count));
}
