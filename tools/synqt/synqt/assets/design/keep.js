// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What this browser remembers between visits: the pane sizes, and the document.
//
// The pane sizes belong to the person and their screen and never go into the project. They
// are kept as fractions of the window, so a layout carries over to another monitor, and
// clamped when read back, so a value from a large screen cannot leave a pane unusable on a
// small one. They live in localStorage, which is read synchronously before the first paint.
//
// The document matters on synqt.org, where no SynQt is behind the page and closing the tab
// would lose the design. It is a whole project, QML text included, so it goes in IndexedDB,
// which has room for it and restores without blocking the first frame.
//
// When `synqt design` serves this page, the disk is the truth and nothing stored here is
// restored over it.

const PANES = "synqt.design.panes";
const DATABASE = "synqt-design";
const STORE = "documents";
const ONLY = "current";

// Each seam: the property it sets, the window measure it is a fraction of, and the range it
// may occupy on any screen. The bounds are in pixels, because a rail becomes unusable when
// too few characters fit in it, whatever share of the window it holds.
export const PANES_KEPT = [
    {property: "--rail-width", of: "width", least: 150, most: 460, fallback: 224},
    {property: "--inspector-width", of: "width", least: 220, most: 640, fallback: 352},
    {property: "--dock-height", of: "height", least: 120, most: 900, fallback: 0.4},
];

function ofWindow(measure) {
    return measure === "height" ? window.innerHeight : window.innerWidth;
}

// A size in pixels, clamped to what is usable on the window as it is now.
function pixels(pane, share) {
    const whole = ofWindow(pane.of);
    return Math.round(Math.min(pane.most, Math.max(pane.least, share * whole)));
}

export function readPanes() {
    let stored = {};
    try {
        stored = JSON.parse(window.localStorage.getItem(PANES) || "{}") || {};
    } catch (error) {
        stored = {};        // unreadable is the same as unset: lay the panes out afresh
    }
    const sizes = {};
    for (const pane of PANES_KEPT) {
        const share = Number(stored[pane.property]);
        if (Number.isFinite(share) && share > 0 && share < 1) {
            sizes[pane.property] = pixels(pane, share);
        }
    }
    return sizes;
}

export function keepPane(property, size) {
    const pane = PANES_KEPT.find((one) => one.property === property);
    if (!pane) {
        return;
    }
    let stored = {};
    try {
        stored = JSON.parse(window.localStorage.getItem(PANES) || "{}") || {};
    } catch (error) {
        stored = {};
    }
    stored[property] = size / ofWindow(pane.of);
    try {
        window.localStorage.setItem(PANES, JSON.stringify(stored));
    } catch (error) {
        // A browser refusing to store is not a reason to stop resizing panes.
    }
}

// The document, in IndexedDB

function open() {
    return new Promise((resolve, reject) => {
        if (!window.indexedDB) {
            reject(new Error("no indexedDB"));
            return;
        }
        const asked = window.indexedDB.open(DATABASE, 1);
        asked.onupgradeneeded = () => {
            if (!asked.result.objectStoreNames.contains(STORE)) {
                asked.result.createObjectStore(STORE);
            }
        };
        asked.onsuccess = () => resolve(asked.result);
        asked.onerror = () => reject(asked.error);
    });
}

function inStore(mode, work) {
    return open().then((database) => new Promise((resolve, reject) => {
        const deal = database.transaction(STORE, mode);
        const asked = work(deal.objectStore(STORE));
        asked.onsuccess = () => resolve(asked.result);
        asked.onerror = () => reject(asked.error);
        deal.oncomplete = () => database.close();
    }));
}

// Storing is best effort. A browser in private mode, or one whose quota is full, refuses,
// and the editor carries on without reporting a storage error nobody can act on.
export async function keepDesign(design, seed) {
    try {
        await inStore("readwrite", (store) => store.put({
            version: 1,
            design,
            // The example this design started from, if any. Someone who opens an example,
            // changes it and reloads expects their changes, not the example the address bar
            // still names.
            seed: seed || "",
        }, ONLY));
    } catch (error) {
        return false;
    }
    return true;
}

// What this browser holds: the design and the example it started from, or null on a first
// visit.
export async function keptDesign() {
    try {
        const held = await inStore("readonly", (store) => store.get(ONLY));
        return held && held.design ? {design: held.design, seed: held.seed || ""} : null;
    } catch (error) {
        return null;
    }
}

export async function forgetDesign() {
    try {
        await inStore("readwrite", (store) => store.delete(ONLY));
    } catch (error) {
        // Nothing stored is the state this was asking for anyway.
    }
}
