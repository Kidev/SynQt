-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
-- SPDX-License-Identifier: Apache-2.0

-- The durable Hall of Fame (docs/tutorial-hall-of-fame.md). Applied by the persistence
-- blueprint at startup; forward-only migrations extend it.
CREATE TABLE IF NOT EXISTS winners (
    id     INTEGER PRIMARY KEY,
    item   TEXT NOT NULL,
    winner TEXT NOT NULL,
    amount INTEGER NOT NULL
);
