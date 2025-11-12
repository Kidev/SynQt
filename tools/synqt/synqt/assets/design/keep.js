// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What this browser remembers between visits, and the two very different reasons it does.
//
// The pane sizes belong to the person and their screen. They are three numbers, they are read
// before the first paint, and they must never end up in the project. A colleague pulling the
// repository should not inherit somebody else's idea of how wide a sidebar is. They are kept
// as *fractions* of the window rather than pixels, so a layout arranged on one monitor is
// still the same layout on another, and clamped on the way back in so a stored value from a
// very large screen cannot leave a pane unusable on a small one. localStorage, because three
// numbers read synchronously at start-up is exactly what it is for.
//
// The document belongs to the work. On synqt.org there is no SynQt behind the page, so a
// design lives only in the tab it was drawn in, and closing that tab has been enough to lose
// an afternoon. It is a whole project, QML text and all, so it goes in IndexedDB: room to
// grow, and asynchronous, so restoring it never blocks the first frame.
//
// The one thing neither of them may ever do is show somebody a project that is not the
// project. When `synqt design` is serving this page, the disk is the truth and nothing stored
// here is restored over it.

const PANES = "synqt.design.panes";
const DATABASE = "synqt-design";
const STORE = "documents";
const ONLY = "current";

// Each seam. The property it sets, the window measure it is a fraction of, and the range it
// is allowed to occupy on any screen. The bounds are in pixels, because what makes
// a rail unusable is how few characters fit in it, not what share of the window it holds.
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

// Storing is best-effort on purpose. A browser in private mode, or one whose quota is full,
// refuses, and the right answer to that is to carry on drawing rather than to interrupt
// somebody mid-thought with a storage error they cannot act on.
export async function keepDesign(design) {
    try {
        await inStore("readwrite", (store) => store.put({
            version: 1,
            design,
        }, ONLY));
    } catch (error) {
        return false;
    }
    return true;
}

export async function keptDesign() {
    try {
        const held = await inStore("readonly", (store) => store.get(ONLY));
        return held && held.design ? held.design : null;
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
