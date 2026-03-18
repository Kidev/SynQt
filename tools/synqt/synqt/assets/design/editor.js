// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The file pane's editor.
//
// It used to be two layers sharing a box: a transparent textarea holding the caret, and a
// coloured copy of the same text painted behind it. That works until it does not, and the
// list of what it could never do is the list of what anybody expects of a pane they type code
// into -- line numbers, a matched bracket, a selection that is visible, an undo of its own.
// Every one of those was a layer with nothing to build it on.
//
// This is CodeMirror doing that job; vendor/README.md is what is vendored and how. The
// colouring is still ours: `runsFor` in source.js is the one description of what QML, YAML,
// SQL, a contract, and a page an entity serves (HTML with CSS and JavaScript in it) look
// like, and this turns its runs into decorations. Nothing about the syntax is written twice,
// so the pane and the canvas cannot drift apart -- the members written along a link and the
// file they are declared in are coloured by one reader.

import { Compartment, EditorState, RangeSetBuilder } from "./vendor/codemirror-state.js";
import { Decoration, EditorView, ViewPlugin, crosshairCursor, drawSelection,
         highlightActiveLine, highlightActiveLineGutter, highlightSpecialChars, keymap,
         lineNumbers, rectangularSelection } from "./vendor/codemirror-view.js";
import { defaultKeymap, history, historyKeymap } from "./vendor/codemirror-commands.js";
import { bracketMatching } from "./vendor/codemirror-language.js";
import { runsFor } from "./source.js";

// The pane, styled.
//
// A CodeMirror theme and not rules in editor.css, which is where this started and where it
// did not work. The base theme the library ships with is two and three classes deep
// (a generated class of its own, then `.cm-gutters`; a generated class, `.cm-lineNumbers` and
// `.cm-gutterElement`), and a plain `.cm-gutters` in a stylesheet loses to every one of them. That was not a detail of the gutter. The pane had been wearing
// CodeMirror's *light* base theme on a dark page, which is why the number of the line the
// caret was on sat invisible on a pale blue block, and why the selection and the caret were
// the library's colours rather than the page's. A theme is the mechanism the library provides
// for this and it outranks the base theme by construction, so nothing here has to out-guess a
// specificity.
//
// The values are the page's own custom properties. They cross into the shadow root and are
// resolved where they are used, so this follows the reader's light or dark setting with no
// rebuild -- which is the one thing a theme written in JavaScript would otherwise cost.
const PANE_THEME = EditorView.theme({
    "&": {
        height: "100%",
        backgroundColor: "var(--page)",
        color: "var(--ink)",
        fontFamily: "var(--mono)",
        fontSize: "12px",
    },
    // No ring around the pane when the caret is in it. Which pane has the caret is said by
    // the caret, and an outline the width of the files pane is a box drawn around the whole
    // project.
    "&.cm-focused": {outline: "none"},
    ".cm-scroller": {fontFamily: "inherit", lineHeight: "1.45"},
    ".cm-content": {padding: "0.75rem 0", caretColor: "var(--accent)"},

    // The line numbers, which is half of what this pane gained by becoming an editor: a
    // finding naming a line, a stack trace, and a colleague saying "look at line 40" all
    // point at something now.
    //
    // An index and not the file, so they are set quiet, in figures of one width so a column
    // of them is a column, and with no rule down the side. A border between the numbers and
    // the code drew a second edge a few pixels from the one the tree already draws, and the
    // gutter is not a pane.
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
    // The line the caret is on, said by its number rather than by a block behind it. The
    // block is what the base theme does, and on this page it was a pale blue rectangle with
    // the digit lost inside it.
    ".cm-activeLineGutter": {backgroundColor: "transparent"},
    ".cm-activeLineGutter .cm-gutterElement, &.cm-focused .cm-activeLineGutter": {
        backgroundColor: "transparent",
    },
    "&.cm-focused .cm-lineNumbers .cm-activeLineGutter": {
        color: "var(--accent)",
        opacity: "1",
        fontWeight: "600",
    },

    // The line the caret is on, marked rather than lit. A filled band across a pane this
    // narrow reads as a selection, and the pane already has one of those.
    ".cm-activeLine": {
        backgroundColor: "color-mix(in srgb, var(--panel-high) 55%, transparent)",
    },
    // A file being read rather than typed into. The lock is said on the bar above in words;
    // this is the pane agreeing with it, quietly enough that a locked file is still a file
    // to read.
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

    // The other half of what it gained. The brace at the far end of the block the caret is
    // in. QML is nested objects, and the one question a reader has of a closing brace is
    // which opening one it answers.
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

// The class a run of a given kind is painted with. The same names the stylesheet has used all
// along, so one set of rules colours the pane, the canvas and the cards.
const MARKS = new Map();

function markFor(kind) {
    if (!MARKS.has(kind)) {
        MARKS.set(kind, Decoration.mark({class: `tok tok--${kind}`}));
    }
    return MARKS.get(kind);
}

// The whole document at once rather than only what is on screen. This reader is not local: a
// block scalar in synqt.yaml decides how every line under it is read, and a viewport that
// begins in the middle of one has no way to know it is in one. The files here are a few
// hundred lines, so reading all of them costs less than being wrong about any of them.
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
// the file it is about: one editor with one growing history let a reader open a file, press
// undo, and watch the file before it arrive in the pane.
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
// `onInput` is every edit somebody makes and never a change this page wrote: opening a file
// and reading a design back both replace the whole document, and reporting those as edits is
// how a page reading a file ends up editing it.
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

    // Inside a shadow root, which is what makes a vendored editor and a strict policy live
    // together. CodeMirror builds its own stylesheet at runtime. Against a document, style-mod
    // builds it as a <style> element with text in it, which a page served under
    // `default-src 'none'` refuses outright, and against a shadow root it builds it as a
    // constructed CSSStyleSheet, which no policy has an opinion about. The library's styles
    // being scoped away from the rest of the page comes free with it.
    //
    // Open, so anything outside can still reach in. The shadow is here to keep a stylesheet
    // in, not to keep a reader out.
    const root = parent.attachShadow({mode: "open"});
    // Linked rather than inlined for the same reason the boundary is here at all: a
    // stylesheet fetched from this origin is what the policy allows, and one written into the
    // page is what it does not.
    const sheet = document.createElement("link");
    sheet.rel = "stylesheet";
    sheet.href = "editor.css";
    root.append(sheet);

    const view = new EditorView({parent: root, root, state: stateFor("", true)});
    // The editor, on the element it is built into, which is the one handle anything outside
    // this module has on the file the pane is holding. It needs one: an editor renders the
    // lines that are on screen and no others, so the pane's DOM is a window onto a file and
    // never the file. CodeMirror's own `EditorView.findFromDOM` is the same idea.
    parent.editor = view;

    return {
        // Show `text` as the file called `named`. The pane is rebuilt from the document on
        // every keystroke, so the common call is the file it already holds with the text it
        // already has. That one changes nothing, which is what keeps the caret where it is.
        show(named, text, readOnly) {
            quiet = true;
            if (named !== name) {
                name = named;
                locked = readOnly;
                view.setState(stateFor(text, readOnly));
                // A new state starts at the top. The box it is rendered in does not, and a
                // file opened after a long one arrived scrolled to where the last one was
                // left with its first lines above the frame.
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
