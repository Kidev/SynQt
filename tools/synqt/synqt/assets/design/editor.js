// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The file pane's editor.
//
// CodeMirror, for line numbers, bracket matching, a visible selection and an undo history per
// file. vendor/README.md says what is vendored and how. The colouring is SynQt's own:
// `runsFor` in source.js is the one description of what QML, YAML, SQL, a contract and a page
// an entity serves (HTML with CSS and JavaScript in it) look like, and this turns its runs
// into decorations. The syntax is described once, so the pane and the canvas cannot drift.

import { Compartment, EditorState, RangeSetBuilder } from "./vendor/codemirror-state.js";
import { Decoration, EditorView, ViewPlugin, crosshairCursor, drawSelection,
         highlightActiveLine, highlightActiveLineGutter, highlightSpecialChars, keymap,
         lineNumbers, rectangularSelection } from "./vendor/codemirror-view.js";
import { defaultKeymap, history, historyKeymap } from "./vendor/codemirror-commands.js";
import { bracketMatching } from "./vendor/codemirror-language.js";
import { runsFor } from "./source.js";

// The pane, styled.
//
// A CodeMirror theme rather than rules in editor.css: the library's base theme is two and
// three classes deep, and a theme outranks it by construction.
//
// The values are the page's own custom properties. They cross into the shadow root and are
// resolved where they are used, so the theme follows the light or dark setting with no
// rebuild.
const PANE_THEME = EditorView.theme({
    "&": {
        height: "100%",
        backgroundColor: "var(--page)",
        color: "var(--ink)",
        fontFamily: "var(--mono)",
        fontSize: "12px",
    },
    // No ring around the pane when the caret is in it: the caret already shows where focus
    // is.
    "&.cm-focused": {outline: "none"},
    ".cm-scroller": {fontFamily: "inherit", lineHeight: "1.45"},
    ".cm-content": {padding: "0.75rem 0", caretColor: "var(--accent)"},

    // The line numbers: dimmed, in tabular figures so they line up, and with no border,
    // which would draw a second edge a few pixels from the one the tree already draws.
    ".cm-gutters": {
        backgroundColor: "var(--page)",
        color: "var(--ink-dim)",
        border: "none",
    },
    ".cm-lineNumbers .cm-gutterElement": {
        minWidth: "2.4em",
        padding: "0 0.7rem 0 0.85rem",
        fontVariantNumeric: "tabular-nums",
        opacity: "0.55",
    },
    // The line the caret is on, said by its number rather than by a block behind it: the
    // base theme's pale block hides the digit inside it.
    ".cm-activeLineGutter": {backgroundColor: "transparent"},
    ".cm-activeLineGutter .cm-gutterElement, &.cm-focused .cm-activeLineGutter": {
        backgroundColor: "transparent",
    },
    "&.cm-focused .cm-lineNumbers .cm-activeLineGutter": {
        color: "var(--accent)",
        opacity: "1",
        fontWeight: "600",
    },

    // The line the caret is on, marked faintly: a filled band across a pane this narrow
    // reads as a selection.
    ".cm-activeLine": {
        backgroundColor: "color-mix(in srgb, var(--panel-high) 55%, transparent)",
    },
    // No active line while the pane is not focused, as when a locked file is being read.
    "&:not(.cm-focused) .cm-activeLine": {backgroundColor: "transparent"},

    ".cm-cursor, .cm-dropCursor": {
        borderLeftColor: "var(--accent)",
        borderLeftWidth: "2px",
    },
    ".cm-selectionBackground": {
        background: "color-mix(in srgb, var(--accent) 34%, transparent)",
    },
    "&.cm-focused > .cm-scroller > .cm-selectionLayer .cm-selectionBackground": {
        background: "color-mix(in srgb, var(--accent) 44%, transparent)",
    },
    ".cm-content ::selection, .cm-line::selection": {
        background: "color-mix(in srgb, var(--accent) 44%, transparent)",
    },

    // The brace matching the one at the caret. QML is nested objects, so this is how a
    // reader finds which opening brace a closing one answers.
    "&.cm-focused .cm-matchingBracket, .cm-matchingBracket": {
        backgroundColor: "color-mix(in srgb, var(--accent) 22%, transparent)",
        outline: "1px solid color-mix(in srgb, var(--accent) 55%, transparent)",
        color: "inherit",
    },
    "&.cm-focused .cm-nonmatchingBracket, .cm-nonmatchingBracket": {
        backgroundColor: "color-mix(in srgb, var(--error) 22%, transparent)",
        color: "inherit",
    },
    ".cm-specialChar": {color: "var(--error)"},
});

// The class a run of a given kind is painted with, the same names the canvas and the cards
// use, so one set of rules colours all three.
const MARKS = new Map();

function markFor(kind) {
    if (!MARKS.has(kind)) {
        MARKS.set(kind, Decoration.mark({class: `tok tok--${kind}`}));
    }
    return MARKS.get(kind);
}

// The whole document at once, not only what is on screen. This reader is not local: a block
// scalar in synqt.yaml decides how every line under it is read, and a viewport that begins in
// the middle of one cannot know it. The files here are a few hundred lines.
function painted(state, named) {
    const builder = new RangeSetBuilder();
    let at = 0;
    for (const run of runsFor(named(), state.doc.toString())) {
        const to = at + run.text.length;
        if (run.kind && to > at) {
            builder.add(at, to, markFor(run.kind));
        }
        at = to;
    }
    return builder.finish();
}

function painter(named) {
    return ViewPlugin.fromClass(class {
        constructor(view) {
            this.decorations = painted(view.state, named);
        }

        update(update) {
            if (update.docChanged) {
                this.decorations = painted(update.state, named);
            }
        }
    }, {decorations: (plugin) => plugin.decorations});
}

// Everything the editor is, per file. Built fresh for each one so the undo history belongs to
// that file: with one shared history, undo in a newly opened file would bring back the
// previous one.
function extensionsFor(named, editable, readOnly, watch) {
    return [
        lineNumbers(),
        highlightActiveLineGutter(),
        highlightActiveLine(),
        highlightSpecialChars(),
        drawSelection(),
        rectangularSelection(),
        crosshairCursor(),
        history(),
        bracketMatching(),
        keymap.of([...defaultKeymap, ...historyKeymap]),
        PANE_THEME,
        EditorState.allowMultipleSelections.of(true),
        // Four, which is what every SynQt file is written with and what qmlformat writes back.
        EditorState.tabSize.of(4),
        painter(named),
        editable.of(EditorState.readOnly.of(readOnly)),
        watch,
    ];
}

// One editor, made once and given a file at a time.
//
// `onInput` reports every edit a person makes and never a change this page wrote: opening a
// file and reading a design back both replace the whole document, and reporting those as
// edits would write the file back.
export function makeEditor({parent, onInput, onCaret}) {
    let name = "";
    let locked = true;
    let quiet = false;
    const editable = new Compartment();

    const watch = EditorView.updateListener.of((update) => {
        if (quiet) {
            return;
        }
        if (update.docChanged && onInput) {
            onInput(update.state.doc.toString());
        } else if (update.selectionSet && onCaret) {
            onCaret();
        }
    });

    const stateFor = (text, readOnly) => EditorState.create({
        doc: text,
        extensions: extensionsFor(() => name, editable, readOnly, watch),
    });

    // Inside a shadow root, so the vendored editor runs under the strict policy. CodeMirror
    // builds its own stylesheet at run time: against a document, style-mod writes a <style>
    // element, which a page served under `default-src 'none'` refuses, and against a shadow
    // root it builds a constructed CSSStyleSheet, which the policy allows. It also scopes the
    // library's styles away from the rest of the page.
    //
    // Open, so code outside can still reach in. The shadow only keeps a stylesheet in.
    const root = parent.attachShadow({mode: "open"});
    // Linked, not inlined: the policy allows a stylesheet fetched from this origin and refuses
    // one written into the page.
    const sheet = document.createElement("link");
    sheet.rel = "stylesheet";
    sheet.href = "editor.css";
    root.append(sheet);

    const view = new EditorView({parent: root, root, state: stateFor("", true)});
    // The editor, on the element it is built into: the handle code outside this module has on
    // the file in the pane. The editor renders only the lines on screen, so the pane's DOM is
    // never the whole file. CodeMirror's own `EditorView.findFromDOM` is the same idea.
    parent.editor = view;

    return {
        // Show `text` as the file called `named`. The pane is rebuilt from the document on
        // every keystroke, so the common call passes the file and text it already holds, and
        // that changes nothing, which keeps the caret where it is.
        show(named, text, readOnly) {
            quiet = true;
            if (named !== name) {
                name = named;
                locked = readOnly;
                view.setState(stateFor(text, readOnly));
                // A new state starts at the top, and the box it is rendered in is scrolled
                // there too, or a file opened after a long one keeps the old scroll offset.
                view.scrollDOM.scrollTop = 0;
                view.scrollDOM.scrollLeft = 0;
            } else {
                if (view.state.doc.toString() !== text) {
                    const held = view.state.selection.main;
                    view.dispatch({
                        changes: {from: 0, to: view.state.doc.length, insert: text},
                        selection: {anchor: Math.min(held.anchor, text.length),
                                    head: Math.min(held.head, text.length)},
                    });
                }
                if (locked !== readOnly) {
                    locked = readOnly;
                    view.dispatch({
                        effects: editable.reconfigure(EditorState.readOnly.of(readOnly)),
                    });
                }
            }
            quiet = false;
        },

        // Which line the caret is on, counted from zero the way every other line number here
        // is counted.
        caretLine() {
            return view.state.doc.lineAt(view.state.selection.main.head).number - 1;
        },

        focus() {
            view.focus();
        },
    };
}
