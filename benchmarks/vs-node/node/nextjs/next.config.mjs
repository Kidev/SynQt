// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The Next.js column of the comparison, configured for one job: answering the six
// TechEmpower routes and one live route the same way the other columns answer them.
const config = {
    // The six answers and the seeded database come from ../techempower.mjs, which is the
    // one file every column reads them out of. It is outside this app's directory on
    // purpose: a copy of it in here is a copy that drifts, and then a difference between
    // two columns of this table is a difference between two fortunes lists.
    experimental: {
        externalDir: true,
    },
    // A native addon, which no bundler can bundle: it has to be required at run time from
    // node_modules the way the Fastify column requires it.
    serverExternalPackages: ["better-sqlite3"],
    // Nothing here is a page a browser reads, so the framework's own instrumentation is
    // off: what is being measured is the request path, not the telemetry on it.
    poweredByHeader: false,
};

export default config;
