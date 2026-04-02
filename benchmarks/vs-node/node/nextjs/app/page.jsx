// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import {Caller} from "./caller.jsx";

// The page exists so the Server Functions in ./actions.js have somewhere to be reachable
// from. A server action is addressed by an id `next build` assigns and is served on the
// route of a page whose module graph contains it, so a page that imports nothing that
// imports the actions would leave them with no route and no id -- and the column measuring
// them would be measuring a 404.
//
// The client component under it is what puts the actions in that graph. It renders nothing
// worth looking at: a browser is not what drives this benchmark, the harness in
// ../../calls-nextjs.mjs is, and it POSTs to this route exactly as React's client runtime
// would.
export default function Home() {
    return (
        <main>
            The Next.js column of the SynQt comparison. See ../README.md.
            <Caller />
        </main>
    );
}
