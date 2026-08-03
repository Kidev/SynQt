-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
-- SPDX-License-Identifier: Apache-2.0

CREATE TABLE IF NOT EXISTS winners (
    id     INTEGER PRIMARY KEY,
    item   TEXT NOT NULL,
    winner TEXT NOT NULL,
    amount INTEGER NOT NULL
);
