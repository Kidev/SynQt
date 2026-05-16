// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import {randomWorldId} from "../../../techempower.mjs";
import {store} from "../store.js";

export const dynamic = "force-dynamic";

export function GET() {
    const id = randomWorldId();
    return Response.json({id, randomNumber: store().selectWorld.get(id).randomNumber});
}
