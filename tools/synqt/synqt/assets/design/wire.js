// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// How big the wire is: what a contract's members cost in bytes when they cross the link.
//
// A connect point says what crosses it, and this says how much. A model of eight roles with a
// `string[280]` beside it could be a packet or a page, and that decides whether a property is
// pushed on every keystroke or on a timer.
//
// The numbers are QDataStream's, which is what QtRemoteObjects serialises with, and they are
// bounds, not measurements. A `string[60]` is 4 bytes of length and at most 60 UTF-16 code
// units, so 124, and a shorter one is shorter. A member is "at most" a number when every part
// of it is bounded, and unbounded when one part is not. An unsized `string` has no ceiling,
// so none is invented for it.
//
// Nothing here is a rule: `synqt check` does not limit how big a contract is, and neither
// does the drawing. It is a reading, shown where the contract is written.

// What one value of each type costs, serialised.
//
// QDataStream writes a QString as a quint32 byte count followed by UTF-16, so a bounded
// string of n characters is 4 + 2n. A QUrl goes across as the QString of its text. A
// QDateTime is a qint64 Julian day, a quint32 of milliseconds and a qint8 of the time spec.
// A QVariant carries its type id and a null flag in front of the value, which is the 5 bytes
// `wrapped` adds.
const FIXED = {
    bool: 1,
    int: 4,
    double: 8,
    real: 8,
    date: 13,
};

// The bytes a QVariant puts in front of the value it holds. Quint32 type id, quint8 null flag.
const VARIANT_HEAD = 5;

// The bytes a QString or a QUrl puts in front of its characters, and what one character costs.
const STRING_HEAD = 4;
const STRING_CHAR = 2;

// What QtRemoteObjects wraps one packet in: a quint32 length, a quint16 packet type, the
// remoted object's name as a QString, and a qint32 (the property index of a change, the
// serial id a reply answers). The name is the one part that varies, so it is measured rather
// than assumed.
const PACKET_FIXED = 4 + 2 + STRING_HEAD + 4;

// An invoke packet, which carries a signal emitted and a slot called, has four more qint32s
// around its arguments: the call type, the argument count, the serial id and the property
// index. Each argument goes as a QVariant (QDataStreamCodec::serializeInvokePacket).
const INVOKE_EXTRA = 4 * 4;

// What one row of a replicated model costs beyond its roles. The row index and the count of
// roles in it.
const ROW_FIXED = 8;

// A size, in a unit readable at a glance. SI rather than binary, because this is a quantity
// of bytes on a network, not a block on a disk.
export function sizeText(bytes) {
    if (bytes < 1000) {
        return `${Math.round(bytes)} B`;
    }
    if (bytes < 1000 * 1000) {
        return `${(bytes / 1000).toFixed(bytes < 10000 ? 1 : 0)} kB`;
    }
    return `${(bytes / (1000 * 1000)).toFixed(1)} MB`;
}

// One written type, split into its base and the bound it was written with. The same shape
// synqtc.types.parse_type reads, and the same answer.
function parsed(type) {
    const match = /^([A-Za-z_][A-Za-z0-9_]*)(?:\[([0-9]+)\])?$/.exec(String(type || "").trim());
    if (!match) {
        return {base: "", size: null};
    }
    return {base: match[1], size: match[2] === undefined ? null : Number(match[2])};
}

// What one value of `type` costs, and whether that is a ceiling or an open end.
//
// `var[n]` is the one type whose bound is already the wire measure: the contract compiler
// holds it with synqtVariantBytes, which is what the value serialises to, so the bound is the
// answer rather than something to build one out of. A `list[n]` bounds how many elements
// there are and says nothing about what is in them, so it is bounded in count and unbounded
// in bytes.
export function valueBytes(type) {
    const {base, size} = parsed(type);
    if (FIXED[base] !== undefined) {
        return {bytes: FIXED[base], bounded: true};
    }
    if (base === "string" || base === "url") {
        return size === null
            ? {bytes: STRING_HEAD, bounded: false}
            : {bytes: STRING_HEAD + (STRING_CHAR * size), bounded: true};
    }
    if (base === "var" || base === "variant") {
        return size === null ? {bytes: VARIANT_HEAD, bounded: false}
                             : {bytes: size, bounded: true};
    }
    if (base === "list") {
        return {bytes: STRING_HEAD + ((size || 0) * VARIANT_HEAD), bounded: false};
    }
    // A record, or a type this reader does not know. Either way it is not something to
    // invent a number for.
    return {bytes: 0, bounded: false};
}

// The two together, so a caller can add a list of them up without unpacking each one.
function add(into, one) {
    return {bytes: into.bytes + one.bytes, bounded: into.bounded && one.bounded};
}

// What one member costs each time it crosses, and what "once" means for it.
//
// A property is one packet per change. A signal or a slot call is one packet per call,
// carrying the parameters. A slot that answers sends its return value back in a packet of its
// own, so both are counted. A model is counted per row: how many rows there are is the
// application's business, and the contract only says what a row holds.
export function memberBytes(member, link) {
    const named = String((link && link.owner) || "");
    const packet = PACKET_FIXED + (STRING_CHAR * named.length);
    const kind = String((member && member.kind) || "");
    if (kind === "model") {
        const roles = (member.roles || []).reduce(
            (into, role) => add(into, add(valueBytes(role.type), {bytes: VARIANT_HEAD,
                                                                  bounded: true})),
            {bytes: ROW_FIXED, bounded: true});
        return {...roles, per: "row"};
    }
    if (kind === "prop") {
        return {...add({bytes: packet, bounded: true},
                       add(valueBytes(member.type), {bytes: VARIANT_HEAD, bounded: true})),
                per: "change"};
    }
    // The call, and what is in it. The packet is where the sum starts because a call with no
    // parameters still costs one.
    const params = (member.params || []).reduce(
        (into, param) => add(into, add(valueBytes(param.type), {bytes: VARIANT_HEAD,
                                                                bounded: true})),
        {bytes: packet + INVOKE_EXTRA, bounded: true});
    if (kind === "signal") {
        return {...params, per: "emit"};
    }
    // A slot: the call out, and the answer back where the slot has a return type.
    const answered = String(member.type || "")
        ? add(params, add({bytes: packet, bounded: true},
                          add(valueBytes(member.type), {bytes: VARIANT_HEAD, bounded: true})))
        : params;
    return {...answered, per: "call"};
}

// The whole contract as one number: what crosses when every member crosses once, with a
// model counted as one row. It is neither a rate nor a total, since how often each member
// crosses is the application's business.
export function contractBytes(link) {
    return (link && link.members || []).reduce(
        (into, member) => add(into, memberBytes(member, link)),
        {bytes: 0, bounded: true});
}

// A member's cost, in the words the panel says it in.
export function memberSizeText(member, link) {
    const cost = memberBytes(member, link);
    const per = cost.per === "row" ? " per row" : "";
    return cost.bounded ? `at most ${sizeText(cost.bytes)}${per}`
                        : `over ${sizeText(cost.bytes)}${per}, no limit`;
}
