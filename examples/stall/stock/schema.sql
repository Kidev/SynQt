-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
-- SPDX-License-Identifier: Apache-2.0

-- The durable stock. Applied by the persistence blueprint at startup; forward-only
-- migrations extend it. The sku is the internal key the edge fills its catalog from; it
-- never crosses to the browser-facing offers model.
CREATE TABLE IF NOT EXISTS items (
    sku   TEXT PRIMARY KEY,
    title TEXT NOT NULL,
    price INTEGER NOT NULL
);
