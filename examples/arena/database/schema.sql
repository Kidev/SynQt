-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
-- SPDX-License-Identifier: Apache-2.0

-- The all-time Hall of Fame (docs/tutorial-multiplayer-rounds.md). One row per champion,
-- keyed by their stable GitHub sub, applied by the persistence blueprint at startup.
CREATE TABLE IF NOT EXISTS champions (
    sub    TEXT PRIMARY KEY,
    name   TEXT NOT NULL,
    points INTEGER NOT NULL DEFAULT 0
);
