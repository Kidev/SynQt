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
    // What SynQt itself puts in QML scope. None of these can be an entity accessor, because
    // `addcontract.ALWAYS_RESERVED` refuses an entity these names in the first place. `Server`
    // is deliberately not here: it is the one the client reaches its edge through.
    "Api", "App", "Cache", "Caller", "Client", "Db", "Docs", "EntityTest", "Graphics",
    "Http", "IdentityMapping", "Jobs", "PageSeed", "Router", "Session",
]);

// The declaration forms. All three are one line, which is the whole reason this can run on
// every keystroke: what follows a `function` line is a body, and nothing here reads it.
const PROPERTY = /^[ \t]*(?:(?:readonly|required|default)[ \t]+)*property[ \t]+([A-Za-z_][\w.]*)[ \t]+([A-Za-z_]\w*)/;
// The parentheses are optional because QML makes them optional and qmlformat takes them off:
// `signal closed()` is written back as `signal closed`, and a reader that wanted them would
// have lost the member the first time somebody formatted the file.
const SIGNAL = /^[ \t]*signal[ \t]+([A-Za-z_]\w*)[ \t]*(?:\(([^)]*)\))?/;
const FUNCTION = /^[ \t]*function[ \t]+([A-Za-z_]\w*)[ \t]*\(([^)]*)\)[ \t]*(?::[ \t]*([A-Za-z_]\w*))?/;

// `Owner.member`, which is how an entity reaches something another entity owns: the
// accessor is the owner's name capitalised (`database` is `Database`), or `Server` on the
// client, which is the alias for the edge it reaches. An entity has one connect point, so
// the accessor is the whole address and what follows it is a member.
const REFERENCE = /\b([A-Z][A-Za-z0-9_]*)\.([A-Za-z_]\w*)[ \t]*(\()?/g;

// An import names a module, not an entity, and `import QtQuick.Controls` matches the shape
// above exactly. Taken out before the scan rather than filtered after it, because the list
// of module names nobody could ever call an entity is not one anybody can finish writing.
const IMPORT_LINE = /^[ \t]*(?:import|pragma)\b.*$/gm;

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

// The `export: |` block, whose lines are a contract and not YAML. Everything a connect point
// carries is written in there, in the same grammar a `.syn` file is written in, so it is
// coloured by the same reader. `prop`, `model`, `signal` and `slot` are keywords here and a
// type is a type, which is what makes an export block scannable at the size it is set in.
// Left as one flat scalar it was the one part of the configuration that mattered most and
// read as a wall.
const BLOCK_KEY = /^(\s*)export\s*:\s*[|>][-+]?\s*$/;

// The configuration. Keys, the scalars beside them, the comments, and the contract inside an
// export block. Enough that the shape of the file is visible, and no more, because nothing
// here has to understand YAML.
function yamlRuns(text) {
    const out = [];
    // The indent of the `export:` key while one is open, or null. A block scalar runs until a
    // line comes back to that indent or further out, which is the whole rule needed
    // here. What is inside is never YAML, so nothing in it has to be read as YAML.
    let block = null;
    for (const line of String(text || "").split("\n")) {
        if (block !== null) {
            const indent = (line.match(/^\s*/) || [""])[0];
            if (!line.trim() || indent.length > block) {
                out.push(...synRuns(line), {text: "\n", kind: ""});
                continue;
            }
            block = null;
        }
        const opens = line.match(BLOCK_KEY);
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
        if (opens) {
            block = opens[1].length;
        }
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

// Every `Owner.member` this file reaches for. An entity has one connect point, so the
// accessor is the whole address and what follows it is a member. `call` is true where it was
// called rather than read, which is the difference between a slot and a prop.
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
    // Blanked rather than removed, so every offset below still points at the same byte of
    // the file and `lineAt` keeps answering with the line somebody can go and open.
    IMPORT_LINE.lastIndex = 0;
    const scanned = source.replace(IMPORT_LINE, (line) => " ".repeat(line.length));
    REFERENCE.lastIndex = 0;
    let match = REFERENCE.exec(scanned);
    while (match !== null) {
        const [, accessor, member, call] = match;
        const key = `${accessor}.${member}`;
        if (!NOT_AN_ENTITY.has(accessor) && !seen.has(key)) {
            seen.add(key);
            found.push({accessor, member, call: Boolean(call),
                        line: lineAt(match.index)});
        }
        match = REFERENCE.exec(scanned);
    }
    return found;
}

// Writing back

// A contract type without its bracketed size. `string[120]` is a string here.
//
// The size is a fact about the boundary and not about the value: the generated owner-side
// code refuses anything longer, and QML has no such type to declare. Writing the brackets
// into the file produced `property string[120] message`, which is not a property with a
// limit on it, it is a syntax error, and the engine refuses the whole document over it.
export function baseType(type) {
    return String(type || "").split("[")[0].trim();
}

export function declarationLine(member) {
    if (member.kind === "prop") {
        return `    property ${baseType(member.type) || UNKNOWN} ${member.name}`;
    }
    const params = (member.params || [])
        .map((param) => `${param.name}: ${baseType(param.type)}`).join(", ");
    if (member.kind === "signal") {
        return params ? `    signal ${member.name}(${params})` : `    signal ${member.name}`;
    }
    const returns = member.type ? `: ${baseType(member.type)}` : "";
    // A body nobody has written yet is a `return;`, which is what a function that answers
    // nothing does. Written out rather than `{}`, because qmlformat expands `{}` to a brace
    // on a line of its own and a project with check.qml_format on reported its own starting
    // files over it. addcontract._declaration writes the same thing.
    return `    function ${member.name}(${params})${returns} {\n        return;\n    }`;
}

// `text` with comments and strings blanked, so a brace counted in it is a brace in the code.
// Every offset is unchanged, because each run is replaced by as many spaces as it held.
function masked(text) {
    return runs(text)
        .map((run) => ((run.kind === "comment" || run.kind === "string")
            ? run.text.replace(/[^\n]/g, " ") : run.text))
        .join("");
}

// The lines one declaration occupies, as `[first, last]` inclusive.
//
// A property and a signal are the line they are written on. A function is that line and
// whatever body follows it, however long, so this counts braces from the first one to the
// one that closes it. Comments and strings are blanked first: a `}` inside either is not a
// brace, and counting it would take half of somebody's function away with the other half.
export function declarationSpan(text, line) {
    const lines = masked(text).split("\n");
    if (line < 0 || line >= lines.length) {
        return null;
    }
    let depth = 0;
    let opened = false;
    for (let at = line; at < lines.length; at += 1) {
        for (const character of lines[at]) {
            if (character === "{") {
                depth += 1;
                opened = true;
            } else if (character === "}") {
                depth -= 1;
            }
        }
        if (opened && depth <= 0) {
            return [line, at];
        }
        if (!opened && at === line) {
            return [line, line];       // nothing was opened, so it is the one line
        }
    }
    return [line, lines.length - 1];
}

// `text` with the declaration on `line` rewritten as `member` now says it.
//
// The signature only. Whatever the author wrote after the opening brace of a function is
// theirs and comes back untouched, and so does the indentation the line was written at,
// which is what keeps this safe to run on a file somebody is in the middle of editing.
export function rewritten(text, line, member) {
    const lines = String(text || "").split("\n");
    if (line < 0 || line >= lines.length) {
        return String(text || "");
    }
    const indent = (lines[line].match(/^[ \t]*/) || [""])[0];
    const written = declarationLine(member).replace(/^[ \t]*/, indent);
    if (member.kind === "slot") {
        const brace = lines[line].indexOf("{");
        const head = written.slice(0, written.lastIndexOf("{"));
        lines[line] = brace < 0 ? written : head + lines[line].slice(brace);
    } else {
        lines[line] = written;
    }
    return lines.join("\n");
}

// `text` with the declaration on `line` taken out, body and all.
export function withoutDeclaration(text, line) {
    const span = declarationSpan(text, line);
    if (!span) {
        return String(text || "");
    }
    const lines = String(text || "").split("\n");
    lines.splice(span[0], (span[1] - span[0]) + 1);
    return lines.join("\n");
}

// The declarations a contract's members would be written as: properties, then signals, then
// the functions, which is the order the QML coding conventions ask for and, now that a body
// is three lines, the only one that reads. A model is skipped. It has no QML form, and
// inventing one would be putting a line in somebody's file that QML would refuse to load.
// addcontract.declarations_for writes the same thing, and a test compares the two.
export function declarationsFor(members) {
    const kept = (members || []).filter((member) => member.kind !== "model" && member.name);
    const groups = [];
    for (const kind of ["prop", "signal"]) {
        const written = kept.filter((member) => member.kind === kind).map(declarationLine);
        if (written.length) {
            groups.push(written.join("\n"));
        }
    }
    groups.push(...kept.filter((member) => member.kind !== "prop" && member.kind !== "signal")
        .map(declarationLine));
    return groups.join("\n\n");
}
