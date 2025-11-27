// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The page's one source reader: where the words of a file are, what a QML file declares, and
// what it reaches for in another entity.
//
// A project is made of three kinds of text and the pane shows all three, so all three are
// coloured here. Only QML is read for meaning, because only QML has any: `synqt infer` does
// that same reading in Python, over the same two questions, and this is it brought to the
// page, because the copy on synqt.org has no CLI behind it and the editor has to behave the
// same in both places.
//
// It is deliberately a reader of declarations, not a parser of QML. A declaration is a line;
// everything below the line is the author's and is never interpreted, never rewritten and
// never held against them. That is what makes it safe to run on every keystroke.
//
// Pure functions over text, no DOM, so the suite can hand them a file with node and compare
// what came back against what `synqt infer` says about the same file.

// What a QML word is, once the line it sits on is known. Only used to paint: nothing here
// decides anything, so a word this gets wrong is a colour, never a member.
const KEYWORDS = new Set([
    "as", "break", "case", "catch", "component", "const", "continue", "default", "delete",
    "do", "else", "enum", "false", "finally", "for", "function", "if", "import", "in",
    "instanceof", "let", "new", "null", "of", "on", "pragma", "property", "readonly",
    "required", "return", "signal", "switch", "this", "throw", "true", "try", "typeof",
    "undefined", "var", "void", "while",
]);

// The declared types a contract and a QML property share, plus the ones QML adds. A word in
// here is painted as a type wherever it appears, which is what a reader is looking for.
const TYPE_WORDS = new Set([
    "bool", "color", "date", "double", "font", "int", "list", "point", "real", "rect",
    "size", "string", "url", "var", "variant", "vector2d", "vector3d",
]);

// Names that are capitalised and are not an entity accessor. Without this list every
// `Math.max(...)` in somebody's QML would read as a connect point on an entity called Math.
const NOT_AN_ENTITY = new Set([
    "Array", "Boolean", "Component", "Date", "JSON", "Json", "Map", "Math", "Number",
    "Object", "Promise", "Qt", "Screen", "Set", "String", "Symbol",
]);

// The declaration forms. All three are one line, which is the whole reason this can run on
// every keystroke: what follows a `function` line is a body, and nothing here reads it.
const PROPERTY = /^[ \t]*(?:(?:readonly|required|default)[ \t]+)*property[ \t]+([A-Za-z_][\w.]*)[ \t]+([A-Za-z_]\w*)/;
const SIGNAL = /^[ \t]*signal[ \t]+([A-Za-z_]\w*)[ \t]*\(([^)]*)\)/;
const FUNCTION = /^[ \t]*function[ \t]+([A-Za-z_]\w*)[ \t]*\(([^)]*)\)[ \t]*(?::[ \t]*([A-Za-z_]\w*))?/;

// `Owner.point.member`, which is how an entity reaches something another entity owns: the
// accessor is the owner's name capitalised (`database` is `Database`), or `Server` on the
// client, which is the alias for the edge it reaches.
const REFERENCE = /\b([A-Z][A-Za-z0-9_]*)\.([a-z][A-Za-z0-9_]*)\.([A-Za-z_]\w*)[ \t]*(\()?/g;

// The one type a reference gives away nothing about. A member read out of a call site is
// known by name and by whether it was called; what it carries is for somebody to say.
const UNKNOWN = "var";

// Painting

// `text` split into runs, each with the kind of word it is, in order and covering every byte.
// The pane paints one span per run, so this is the whole of what the editor knows about how
// QML looks.
export function runs(text) {
    const out = [];
    const source = String(text || "");
    // Sticky and exhaustive: every alternative is anchored at the last match's end, and the
    // final one takes a single character, so the scan cannot stall or skip.
    const scan = /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|("(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'|`(?:[^`\\]|\\.)*`)|(\b\d+(?:\.\d+)?\b)|([A-Za-z_$][\w$]*)|([\s\S])/g;
    let previous = "";
    let found = scan.exec(source);
    while (found !== null) {
        const [whole, comment, string, number, word] = found;
        let kind = "";
        if (comment !== undefined) {
            kind = "comment";
        } else if (string !== undefined) {
            kind = "string";
        } else if (number !== undefined) {
            kind = "number";
        } else if (word !== undefined) {
            kind = wordKind(word, previous, source, scan.lastIndex);
        }
        if (word !== undefined || comment !== undefined || string !== undefined) {
            previous = word === undefined ? "" : word;
        }
        const last = out.at(-1);
        if (last && last.kind === kind) {
            last.text += whole;              // one span per run, not one per character
        } else {
            out.push({text: whole, kind});
        }
        found = scan.exec(source);
    }
    return out;
}

function wordKind(word, previous, source, after) {
    // A word straight after `property` is the type of the property being declared, whatever
    // else that word means elsewhere: `property var rows` declares a var, not a keyword.
    if (previous === "property") {
        return "type";
    }
    if (KEYWORDS.has(word)) {
        return "keyword";
    }
    if (TYPE_WORDS.has(word)) {
        return "type";
    }
    const next = source.slice(after).match(/^[ \t]*(\S)/);
    if (next && next[1] === ":") {
        return "member";                     // a binding, or the name half of `id: root`
    }
    return /^[A-Z]/.test(word) ? "type" : "";
}

// A contract, whose whole vocabulary is four member kinds and a type list. Small enough to
// colour by line, which is also how the pane maps a line back to the member on the canvas.
const SYN_KEYWORDS = new Set(["contract", "record", "prop", "model", "signal", "slot"]);

function synRuns(text) {
    const out = [];
    const scan = /(\/\/[^\n]*)|([A-Za-z_]\w*)|([\s\S])/g;
    let found = scan.exec(String(text || ""));
    while (found !== null) {
        const [whole, comment, word] = found;
        let kind = "";
        if (comment !== undefined) {
            kind = "comment";
        } else if (SYN_KEYWORDS.has(word)) {
            kind = "keyword";
        } else if (TYPE_WORDS.has(word)) {
            kind = "type";
        } else if (word !== undefined && /^[A-Z]/.test(word)) {
            kind = "type";
        }
        const last = out.at(-1);
        if (last && last.kind === kind) {
            last.text += whole;
        } else {
            out.push({text: whole, kind});
        }
        found = scan.exec(String(text || ""));
    }
    return out;
}

// The configuration. Keys, the scalars beside them, and the comments: enough that the shape
// of the file is visible, and no more, because nothing here has to understand YAML.
function yamlRuns(text) {
    const out = [];
    for (const line of String(text || "").split("\n")) {
        const comment = line.indexOf("#");
        const code = comment >= 0 ? line.slice(0, comment) : line;
        const key = code.match(/^(\s*(?:-\s+)?)([A-Za-z_][\w.-]*)(\s*:)/);
        if (key) {
            out.push({text: key[1], kind: ""});
            out.push({text: key[2], kind: "member"});
            out.push({text: key[3], kind: ""});
            out.push({text: code.slice(key[0].length), kind: "string"});
        } else {
            out.push({text: code, kind: ""});
        }
        if (comment >= 0) {
            out.push({text: line.slice(comment), kind: "comment"});
        }
        out.push({text: "\n", kind: ""});
    }
    out.pop();                               // the split added one newline that was not there
    return out;
}

// The runs for whatever kind of file `name` is. An extension nobody colours comes back as one
// plain run, which is the file shown exactly as it is rather than shown wrong.
export function runsFor(name, text) {
    if (String(name).endsWith(".qml")) {
        return runs(text);
    }
    if (String(name).endsWith(".syn")) {
        return synRuns(text);
    }
    if (/\.ya?ml$/.test(String(name))) {
        return yamlRuns(text);
    }
    return [{text: String(text || ""), kind: ""}];
}

// The licence notice, which every SynQt file carries and nobody reads twice. Taken off for
// the pane only. What is written to disk and what the download holds keeps it, because it is
// the thing that makes the file's licence unambiguous wherever it ends up.
export function withoutNotice(text) {
    const lines = String(text || "").split("\n");
    let at = 0;
    while (at < lines.length && /^\s*\/\/\s*SPDX-\w+/.test(lines[at])) {
        at += 1;
    }
    if (!at) {
        return String(text || "");
    }
    while (at < lines.length && !lines[at].trim()) {
        at += 1;                             // and the blank line the notice sat above
    }
    return lines.slice(at).join("\n");
}

// Reading

function paramsOf(text) {
    // Both spellings, because both are QML: `signal denied(string reason)` is the old form
    // and `function load(id: int)` is the annotated one the guide asks for.
    return String(text || "").split(",").map((part) => part.trim()).filter(Boolean)
        .map((part) => {
            const annotated = part.match(/^([A-Za-z_]\w*)[ \t]*:[ \t]*([A-Za-z_]\w*)$/);
            if (annotated) {
                return {type: annotated[2], name: annotated[1]};
            }
            const spaced = part.match(/^([A-Za-z_][\w.]*)[ \t]+([A-Za-z_]\w*)$/);
            return spaced ? {type: spaced[1], name: spaced[2]}
                          : {type: UNKNOWN, name: part.replace(/[^\w]/g, "")};
        })
        .filter((param) => param.name);
}

// Every member `text` declares, as the flat records the document holds, each with the line it
// was read from so the canvas can be pointed at what somebody's cursor is sitting in.
//
// A model is not in here and cannot be. `model rows(int id, string title)` has no QML
// declaration form, so it is the one member kind the panel alone can add.
export function declarations(text) {
    const found = [];
    String(text || "").split("\n").forEach((line, index) => {
        const property = line.match(PROPERTY);
        if (property) {
            found.push({kind: "prop", name: property[2], type: property[1],
                        params: [], roles: [], line: index});
            return;
        }
        const signal = line.match(SIGNAL);
        if (signal) {
            found.push({kind: "signal", name: signal[1], type: "",
                        params: paramsOf(signal[2]), roles: [], line: index});
            return;
        }
        const fn = line.match(FUNCTION);
        if (fn) {
            found.push({kind: "slot", name: fn[1], type: fn[3] || "",
                        params: paramsOf(fn[2]), roles: [], line: index});
        }
    });
    return found;
}

// Every `Owner.point.member` this file reaches for. `call` is true where it was called rather
// than read, which is the difference between a slot and a prop.
export function references(text) {
    const source = String(text || "");
    const lines = [];
    let at = 0;
    for (const line of source.split("\n")) {
        lines.push(at);
        at += line.length + 1;
    }
    const lineAt = (index) => {
        let low = 0;
        while (low + 1 < lines.length && lines[low + 1] <= index) {
            low += 1;
        }
        return low;
    };
    const found = [];
    const seen = new Set();
    REFERENCE.lastIndex = 0;
    let match = REFERENCE.exec(source);
    while (match !== null) {
        const [, accessor, point, member, call] = match;
        const key = `${accessor}.${point}.${member}`;
        if (!NOT_AN_ENTITY.has(accessor) && !seen.has(key)) {
            seen.add(key);
            found.push({accessor, point, member, call: Boolean(call),
                        line: lineAt(match.index)});
        }
        match = REFERENCE.exec(source);
    }
    return found;
}

// Writing back

export function declarationLine(member) {
    if (member.kind === "prop") {
        return `    property ${member.type || UNKNOWN} ${member.name}`;
    }
    if (member.kind === "signal") {
        const params = (member.params || [])
            .map((param) => `${param.name}: ${param.type}`).join(", ");
        return `    signal ${member.name}(${params})`;
    }
    const params = (member.params || [])
        .map((param) => `${param.name}: ${param.type}`).join(", ");
    const returns = member.type ? `: ${member.type}` : "";
    return `    function ${member.name}(${params})${returns} {\n    }`;
}

// The declarations a contract's members would be written as, in the order they are declared.
// A model is skipped: it has no QML form, and inventing one would be putting a line in
// somebody's file that QML would refuse to load.
export function declarationsFor(members) {
    return (members || []).filter((member) => member.kind !== "model" && member.name)
        .map(declarationLine).join("\n");
}
