// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import {clampedQueryCount, randomWorldId} from "../../../techempower.mjs";
import {store} from "../store.js";

export const dynamic = "force-dynamic";

export function GET(request) {
    const {selectWorld} = store();
    const count = clampedQueryCount(
        new URL(request.url).searchParams.get("queries"));
    const rows = [];
    for (let index = 0; index < count; index += 1) {
        const id = randomWorldId();
        rows.push({id, randomNumber: selectWorld.get(id).randomNumber});
    }
    return Response.json(rows);
}
