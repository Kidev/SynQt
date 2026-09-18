// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The page's one source reader: where the words of a file are, what a QML file declares, and
// what it reaches for in another entity.
//
// Every kind of text the pane shows is coloured here: an entity's QML, the configuration, a
// contract, the schema beside a relational entity, and a page an entity serves (markup with a
// stylesheet and a script inside, each read by its own reader).
//
// Only QML is read for meaning. `synqt infer` does the same reading in Python; this is the
// copy for the page, since synqt.org has no CLI behind it.
//
// It reads declarations instead of parsing QML. A declaration is one line, and the body
// below it is never interpreted or rewritten, which keeps it safe to run on every keystroke.
//
// Pure functions over text, no DOM, so the suite can run a file through node and compare the
// result with what `synqt infer` says about the same file.

// The JavaScript keywords, which QML's script half shares with a plain `.js` file.
const JS_KEYWORDS = new Set([
    "as", "async", "await", "break", "case", "catch", "class", "const", "continue",
    "default", "delete", "do", "else", "enum", "export", "extends", "false", "finally",
    "for", "function", "if", "import", "in", "instanceof", "let", "new", "null", "of",
    "return", "static", "super", "switch", "this", "throw", "true", "try", "typeof",
    "undefined", "var", "void", "while", "yield",
]);

// The QML keywords, used only to paint: a word this gets wrong is a colour, never a member.
// The words QML adds (`property`, `on`, `signal`) are ordinary names in JavaScript, so a
// `.js` file is painted without them.
const KEYWORDS = new Set([
    ...JS_KEYWORDS,
    "component", "on", "pragma", "property", "readonly", "required", "signal",
]);

// The types a contract and a QML property share, plus the ones QML adds, painted as types
// wherever they appear.
const TYPE_WORDS = new Set([
    "bool", "color", "date", "double", "font", "int", "list", "point", "real", "rect",
    "size", "string", "url", "var", "variant", "vector2d", "vector3d",
]);

// Capitalised names that are not an entity accessor, so `Math.max(...)` is not read as a
// connect point on an entity called Math.
const NOT_AN_ENTITY = new Set([
    "Array", "Boolean", "Component", "Date", "JSON", "Json", "Map", "Math", "Number",
    "Object", "Promise", "Qt", "Screen", "Set", "String", "Symbol",
    // What SynQt puts in QML scope, which `addcontract.ALWAYS_RESERVED` refuses as entity
    // names. `Server` is not here: it is how the client reaches its edge.
    "Api", "App", "Cache", "Caller", "Client", "Db", "Docs", "EntityTest", "Graphics",
    "Http", "IdentityMapping", "Jobs", "PageSeed", "Router", "Session",
]);

// The declaration forms, each one line. What follows a `function` line is a body, and
// nothing here reads it.
const PROPERTY = /^[ \t]*(?:(?:readonly|required|default)[ \t]+)*property[ \t]+([A-Za-z_][\w.]*)[ \t]+([A-Za-z_]\w*)/;
// The parentheses are optional, as in QML: qmlformat writes `signal closed()` back as
// `signal closed`.
const SIGNAL = /^[ \t]*signal[ \t]+([A-Za-z_]\w*)[ \t]*(?:\(([^)]*)\))?/;
const FUNCTION = /^[ \t]*function[ \t]+([A-Za-z_]\w*)[ \t]*\(([^)]*)\)[ \t]*(?::[ \t]*([A-Za-z_]\w*))?/;

// `Owner.member`, which is how an entity reaches something another entity owns: the
// accessor is the owner's name capitalised (`database` is `Database`), or `Server` on the
// client, which is the alias for the edge it reaches. An entity has one connect point, so
// the accessor is the whole address and what follows it is a member.
const REFERENCE = /\b([A-Z][A-Za-z0-9_]*)\.([A-Za-z_]\w*)[ \t]*(\()?/g;

// An import names a module, not an entity, and `import QtQuick.Controls` matches the shape
// above, so import and pragma lines are blanked before the scan.
const IMPORT_LINE = /^[ \t]*(?:import|pragma)\b.*$/gm;

// An attached signal handler, `Edge.onDenied: reason => ...`, which refers to the signal
// `denied`. The capital after `on` keeps `Server.online` out of it.
const HANDLER = /^on([A-Z]\w*)$/;

// What the framework puts on every consumer facade, which no owner declares. `ready` is
// ConsumerBase's own (true once the replica has finished its handshake).
const FACADE_MEMBERS = new Set(["ready"]);

// The type of a member known only from a call site, which gives away its name and whether
// it was called, and nothing about what it carries.
const UNKNOWN = "var";

// Painting

// `text` split into runs, each with the kind of word it is, in order and covering every
// byte. The pane paints one span per run.
export function runs(text) {
    return scriptRuns(text, KEYWORDS, true);
}

// The same scan over a plain `.js` file: no `property` and no type words. One scanner for
// both, since QML's script half is this language.
function jsRuns(text) {
    return scriptRuns(text, JS_KEYWORDS, false);
}

function scriptRuns(text, keywords, qml) {
    const out = [];
    const source = String(text || "");
    // Sticky and exhaustive. Every alternative is anchored at the last match's end, and the
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
            kind = wordKind(word, previous, source, scan.lastIndex, keywords, qml);
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

function wordKind(word, previous, source, after, keywords, qml) {
    // A word straight after `property` is the type of the property being declared, whatever
    // else that word means elsewhere. `property var rows` declares a var, not a keyword.
    if (qml && previous === "property") {
        return "type";
    }
    if (keywords.has(word)) {
        return "keyword";
    }
    // Only in QML: `url`, `size` and `point` are type names there and ordinary variable
    // names in JavaScript.
    if (qml && TYPE_WORDS.has(word)) {
        return "type";
    }
    const next = source.slice(after).match(/^[ \t]*(\S)/);
    if (next && next[1] === ":") {
        return "member";                     // a binding, or the name half of `id: root`
    }
    return /^[A-Z]/.test(word) ? "type" : "";
}

// A contract, whose vocabulary is four member kinds and a type list.
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

// The `export: |` block, whose lines are a contract, not YAML, so they are coloured by the
// contract reader.
const BLOCK_KEY = /^(\s*)export\s*:\s*[|>][-+]?\s*$/;

// The configuration: keys, the scalars beside them, the comments, and the contract inside an
// export block. Enough to show the shape of the file without understanding YAML.
function yamlRuns(text) {
    const out = [];
    // The indent of the `export:` key while one is open, or null. A block scalar runs until a
    // line comes back to that indent or further out.
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

// The schema beside a relational entity: a list of forward-only statements, painted with a
// small vocabulary and no SQL parser.
const SQL_KEYWORDS = new Set([
    "add", "all", "alter", "and", "as", "asc", "autoincrement", "begin", "between", "by",
    "cascade", "case", "check", "collate", "column", "commit", "conflict", "constraint",
    "create", "cross", "default", "delete", "desc", "distinct", "drop", "else", "end",
    "exists", "foreign", "from", "full", "group", "having", "if", "in", "index", "inner",
    "insert", "into", "is", "join", "key", "left", "like", "limit", "not", "null", "offset",
    "on", "or", "order", "outer", "primary", "references", "rename", "replace", "returning",
    "right", "rollback", "select", "set", "table", "then", "to", "transaction", "trigger",
    "union", "unique", "update", "using", "values", "view", "when", "where", "with",
]);

// The column types SQLite and PostgreSQL spell.
const SQL_TYPES = new Set([
    "bigint", "blob", "boolean", "bytea", "char", "date", "datetime", "decimal", "double",
    "float", "int", "int2", "int4", "int8", "integer", "json", "jsonb", "numeric", "real",
    "serial", "smallint", "text", "time", "timestamp", "timestamptz", "uuid", "varchar",
]);

function sqlRuns(text) {
    const out = [];
    // `--` to the end of the line and `/* */` across lines are both comments. A string is
    // single-quoted and doubles its own quote to escape it. An identifier in double quotes is
    // a name and not a string, so it is left plain.
    const scan = /(--[^\n]*|\/\*[\s\S]*?\*\/)|('(?:[^']|'')*')|(\b\d+(?:\.\d+)?\b)|([A-Za-z_]\w*)|([\s\S])/g;
    let found = scan.exec(String(text || ""));
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
            const lower = word.toLowerCase();
            if (SQL_KEYWORDS.has(lower)) {
                kind = "keyword";
            } else if (SQL_TYPES.has(lower)) {
                kind = "type";
            }
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

// A stylesheet, as in the `<style>` block of a page an entity serves (the monitor's sign-in
// page is two thirds CSS): the comments, the literals, the at-rules, and the name half of a
// declaration.
//
// `color:` and `a:hover` are both a word and a colon. The character before the word decides:
// a declaration follows `{`, `;`, `}` or the start of a line, and `a:hover` in
// `.row a:hover` follows a space inside a selector. A selector at the very start of a line is
// painted as a property name.
const CSS_STARTS = new Set(["", "{", "}", ";"]);

function cssRuns(text) {
    const out = [];
    const source = String(text || "");
    const scan = /(\/\*[\s\S]*?\*\/)|("(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*')|(@[A-Za-z-]+)|(#[0-9A-Fa-f]{3,8}\b|-?\d+(?:\.\d+)?(?:[A-Za-z%]+)?)|([A-Za-z_-][\w-]*)|([\s\S])/g;
    // The last character that was not a space, and whether a newline has been crossed since
    // it. Together they are "what comes before this word", which is what the rule above needs.
    let before = "";
    let fresh = true;
    let found = scan.exec(source);
    while (found !== null) {
        const [whole, comment, string, at, literal, word] = found;
        let kind = "";
        if (comment !== undefined) {
            kind = "comment";
        } else if (string !== undefined) {
            kind = "string";
        } else if (at !== undefined) {
            kind = "keyword";                // @media, @font-face, @import
        } else if (literal !== undefined) {
            kind = "number";                 // a length, a percentage, a hex colour
        } else if (word !== undefined) {
            const next = source.slice(scan.lastIndex).match(/^[ \t]*(\S)/);
            if (next && next[1] === ":" && (fresh || CSS_STARTS.has(before))) {
                kind = "member";
            }
        }
        if (whole.trim()) {
            before = whole.slice(-1);
            fresh = false;
        } else if (whole.indexOf("\n") !== -1) {
            fresh = true;
        }
        const last = out.at(-1);
        if (last && last.kind === kind) {
            last.text += whole;
        } else {
            out.push({text: whole, kind});
        }
        found = scan.exec(source);
    }
    return out;
}

// A page an entity serves a browser that is not the client, such as the monitor's sign-in
// page. Its `<style>` and `<script>` blocks are read by the CSS and JavaScript readers.
const HTML_SCAN = /(<!--[\s\S]*?-->)|(<![A-Za-z][^>]*>)|(<\/?)([A-Za-z][\w-]*)|([\s\S])/g;
const HTML_INSIDE = /(>)|([A-Za-z_:][\w:.-]*)(\s*=\s*)("[^"]*"|'[^']*'|[^\s>]+)?|([\s\S])/y;
// The two elements whose content is not markup, and the reader each one's content is in.
const HTML_BLOCKS = new Map([["style", cssRuns], ["script", jsRuns]]);

function htmlRuns(text) {
    const source = String(text || "");
    const out = [];
    const push = (piece, kind) => {
        if (!piece) {
            return;
        }
        const last = out.at(-1);
        if (last && last.kind === kind) {
            last.text += piece;
        } else {
            out.push({text: piece, kind});
        }
    };
    let at = 0;
    while (at < source.length) {
        HTML_SCAN.lastIndex = at;
        const found = HTML_SCAN.exec(source);
        if (!found) {
            push(source.slice(at), "");
            break;
        }
        push(source.slice(at, found.index), "");
        const [whole, comment, doctype, open, tag] = found;
        at = found.index + whole.length;
        if (comment !== undefined) {
            push(whole, "comment");
            continue;
        }
        if (doctype !== undefined) {
            push(whole, "keyword");
            continue;
        }
        if (tag === undefined) {
            push(whole, "");
            continue;
        }
        push(open, "");
        push(tag, "keyword");
        at = htmlAttributes(source, at, push);
        // `<style>` and `<script>` hold something that is not markup, so their reader takes
        // everything up to the closing tag, or the rest of the file while it is unclosed.
        const inside = open === "<" ? HTML_BLOCKS.get(tag.toLowerCase()) : null;
        if (inside) {
            const closes = source.toLowerCase().indexOf(`</${tag.toLowerCase()}`, at);
            const ends = closes === -1 ? source.length : closes;
            for (const run of inside(source.slice(at, ends))) {
                push(run.text, run.kind);
            }
            at = ends;
        }
    }
    return out;
}

// From just after a tag name to just after the `>` that closes it. The attribute names, the
// quoted values, and the punctuation between them.
function htmlAttributes(source, from, push) {
    HTML_INSIDE.lastIndex = from;
    let found = HTML_INSIDE.exec(source);
    while (found !== null) {
        const [whole, shut, name, equals, value] = found;
        if (shut !== undefined) {
            push(shut, "");
            return HTML_INSIDE.lastIndex;
        }
        if (name !== undefined) {
            push(name, "member");
            push(equals, "");
            push(value || "", "string");
        } else if (/^[A-Za-z_:]/.test(whole)) {
            push(whole, "member");           // a bare attribute: `required`, `autofocus`
        } else {
            push(whole, "");
        }
        found = HTML_INSIDE.exec(source);
    }
    return source.length;
}

// The runs for whatever kind of file `name` is. Any other extension comes back as one plain
// run.
export function runsFor(name, text) {
    if (String(name).endsWith(".qml")) {
        return runs(text);
    }
    if (String(name).endsWith(".syn")) {
        return synRuns(text);
    }
    if (String(name).endsWith(".sql")) {
        return sqlRuns(text);
    }
    if (/\.ya?ml$/.test(String(name))) {
        return yamlRuns(text);
    }
    if (/\.html?$/.test(String(name))) {
        return htmlRuns(text);
    }
    if (String(name).endsWith(".css")) {
        return cssRuns(text);
    }
    if (String(name).endsWith(".js") || String(name).endsWith(".mjs")) {
        return jsRuns(text);
    }
    return [{text: String(text || ""), kind: ""}];
}

// The licence notice, taken off for the pane only. What is written to disk and what the
// download holds keep it.
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
            // An unannotated parameter says nothing about its type, which `untyped` records.
            return spaced ? {type: spaced[1], name: spaced[2]}
                          : {type: UNKNOWN, name: part.replace(/[^\w]/g, ""), untyped: true};
        })
        .filter((param) => param.name);
}

// Every member `text` declares, as the flat records the document holds, each with the line
// it was read from, so the canvas can follow the caret.
//
// A model has no QML declaration form, so only the panel can add one.
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

// Every `Owner.member` this file reaches for. `call` is true where it was called (a slot,
// not a prop), and `handler` where it was listened to (a signal).
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
    // Blanked, not removed, so every offset still points at the same byte of the file.
    IMPORT_LINE.lastIndex = 0;
    const scanned = source.replace(IMPORT_LINE, (line) => " ".repeat(line.length));
    REFERENCE.lastIndex = 0;
    let match = REFERENCE.exec(scanned);
    while (match !== null) {
        const [, accessor, written, call] = match;
        const handled = written.match(HANDLER);
        const member = handled
            ? handled[1][0].toLowerCase() + handled[1].slice(1)
            : written;
        const key = `${accessor}.${member}`;
        if (!NOT_AN_ENTITY.has(accessor) && !FACADE_MEMBERS.has(member) && !seen.has(key)) {
            seen.add(key);
            found.push({accessor, member, call: Boolean(call) && !handled,
                        handler: Boolean(handled), line: lineAt(match.index)});
        }
        match = REFERENCE.exec(scanned);
    }
    return found;
}

// Writing back

// A contract type without its bracketed size. `string[120]` is a string here.
//
// The size is enforced at the boundary by the generated owner-side code, and QML has no sized
// type: `property string[120] message` is a syntax error.
export function baseType(type) {
    return String(type || "").split("[")[0].trim();
}

// A carried member corrected by what the owner's file now declares. The file says the kind,
// the types and the parameters. The contract keeps the size it wrote on any type whose base
// the file did not change, since QML has no sized type to say it with, and keeps its type
// for a parameter the file leaves unannotated.
export function absorbedMember(carried, declared) {
    const keep = (written, base) => (base && baseType(written) === base ? written : base);
    const before = carried.params || [];
    return {
        kind: declared.kind,
        type: keep(carried.type, declared.type),
        params: (declared.params || []).map((param, index) => {
            const was = before[index];
            if (!was) {
                return {type: param.type, name: param.name};
            }
            return {type: param.untyped ? was.type : keep(was.type, param.type),
                    name: param.name};
        }),
    };
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
    // An unwritten body is `return;`, not `{}`, which qmlformat would expand and
    // check.qml_format would then report. addcontract._declaration writes the same.
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
// A property and a signal are the line they are written on. A function is that line and its
// body, so this counts braces from the first one to the one that closes it, with comments
// and strings blanked first.
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
// The signature only. The body after a function's opening brace and the line's indentation
// come back untouched, so this is safe to run on a file being edited.
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

// Where the root object's type name sits in `text`, as `[start, end]`, or null.
//
// The one edit the canvas makes to a file someone wrote: drawing a connect point off an
// entity turns its file into the point's Source, and removing the point turns it back. Only
// the name changes. `synqt/qmlrewrite.py` finds the same span in Python.
export function rootTypeSpan(text) {
    const source = String(text || "");
    // Comments and strings blanked first, so a brace inside either is not the root's.
    const brace = masked(source).indexOf("{");
    if (brace < 0) {
        return null;
    }
    let end = brace;
    while (end > 0 && /\s/.test(source[end - 1])) {
        end -= 1;
    }
    let start = end;
    while (start > 0 && /[A-Za-z0-9_.]/.test(source[start - 1])) {
        start -= 1;
    }
    return start < end ? [start, end] : null;
}

// `text` with its root object's type replaced by `type`.
export function reroot(text, type) {
    const span = rootTypeSpan(text);
    if (!span) {
        return String(text || "");
    }
    return String(text).slice(0, span[0]) + type + String(text).slice(span[1]);
}

// SynQt's pragma for a file there is one of. `synqt build` writes it as `pragma Singleton` in
// the copy the engine loads (synqt/qmlrewrite.py). Both spellings are read here.
export const SHARED_PRAGMA = "Shared";

const PRAGMA_LINE = /^[ \t]*pragma[ \t]+(?:Shared|Singleton)[ \t]*;?[ \t]*(?:\/\/.*)?$/m;

export function isShared(text) {
    return PRAGMA_LINE.test(String(text || ""));
}

// `text` with the pragma on it, after the licence notice and before the first import, the
// only place QML accepts one.
export function withShared(text) {
    const source = String(text || "");
    if (isShared(source)) {
        return source;
    }
    const lines = source.split("\n");
    let at = 0;
    while (at < lines.length && (/^\s*\/\//.test(lines[at]) || !lines[at].trim())) {
        at += 1;
    }
    lines.splice(at, 0, `pragma ${SHARED_PRAGMA}`, "");
    return lines.join("\n");
}

// `text` with the pragma taken off, and the blank line it stood above with it.
export function withoutShared(text) {
    const lines = String(text || "").split("\n");
    const at = lines.findIndex((line) => PRAGMA_LINE.test(line));
    if (at < 0) {
        return String(text || "");
    }
    lines.splice(at, (at + 1 < lines.length && !lines[at + 1].trim()) ? 2 : 1);
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
// functions, as the QML coding conventions order them. A model has no QML form and is
// skipped. addcontract.declarations_for writes the same, and a test compares the two.
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
