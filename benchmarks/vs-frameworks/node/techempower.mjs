// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The six TechEmpower test types, shared by both Node HTTP columns so they differ only in
// the HTTP framework and the database driver, never in what they answer.
//
// The shape is taken from benchmarks/edge/bench_edge.cpp, which is SynQt's column: the same
// 10 000-row world table, the same twelve fortunes including the one with a <script> in it,
// the same 1..500 clamp on the queries count, the same in-memory database. If these three
// files ever disagree about what a route returns, the comparison is measuring the
// disagreement.

export const WORLD_ROWS = 10000;

export const FORTUNES = [
    "fortune: No such file or directory",
    "A computer scientist is someone who fixes things that aren't broken.",
    "After enough decimal places, nobody gives a damn.",
    "A bad random number generator: 1, 1, 1, 1, 1, 4.33e+67, 1, 1, 1",
    "A computer program does what you tell it to do, not what you want it to do.",
    "Emacs is a nice operating system, but I prefer UNIX. (Tom Christiansen)",
    "Any program that runs right is obsolete.",
    "A list is only as strong as its weakest link. (Donald Knuth)",
    "Feature: A bug with seniority.",
    "Computers make very fast, very accurate mistakes.",
    '<script>alert("This should not be displayed in a browser alert box.");</script>',
    "Frameworks come and go; the benchmark abides.",
];

export function randomWorldId() {
    return 1 + Math.floor(Math.random() * WORLD_ROWS);
}

/// The TechEmpower rule: read from the query string, non-numeric or absent is 1, and the
/// result is clamped to 1..500. Getting this wrong is how a benchmark accidentally measures
/// a different test than the one it is being compared against.
export function clampedQueryCount(raw) {
    const value = Number.parseInt(raw, 10);
    if (!Number.isFinite(value)) {
        return 1;
    }
    return Math.min(500, Math.max(1, value));
}

export function escapeHtml(text) {
    return text
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#39;");
}

/// The fortunes page: every stored row plus one added at request time, sorted by message,
/// with the message HTML-escaped. The seeded <script> row is what makes the escaping
/// observable rather than assumed.
export function renderFortunes(rows) {
    const all = [...rows, { id: 0, message: "Additional fortune added at request time." }];
    all.sort((left, right) => (left.message < right.message ? -1
        : left.message > right.message ? 1 : 0));
    let body = "<!DOCTYPE html><html><head><title>Fortunes</title></head><body>"
        + "<table><tr><th>id</th><th>message</th></tr>";
    for (const fortune of all) {
        body += `<tr><td>${fortune.id}</td><td>${escapeHtml(fortune.message)}</td></tr>`;
    }
    return `${body}</table></body></html>`;
}
