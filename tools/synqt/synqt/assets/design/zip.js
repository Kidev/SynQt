// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A zip file, built in the browser out of a handful of text files.
//
// The hosted editor has no disk behind it, so what it offers instead of Apply is a download,
// and this is what the download is. Stored, never deflated: a project is a configuration
// file and a few contracts, compressing them would save nothing worth a compressor, and this
// way the whole format is the handful of records below rather than a dependency.
//
// No DOM and no Blob here, only bytes, so the suite can build one under node and unzip it.

const LOCAL_HEADER = 0x04034b50;
const CENTRAL_HEADER = 0x02014b50;
const END_OF_CENTRAL = 0x06054b50;

// Written by a reader that understands nothing beyond stored entries, which is every one of
// them, and marked as holding UTF-8 names (bit 11).
const VERSION_NEEDED = 20;
const UTF8_NAMES = 0x0800;
const STORED = 0;

// Zip carries an MS-DOS timestamp, and there is no honest one to carry: the same design has
// to produce the same bytes twice, so every entry is dated at the start of the epoch the
// format can express (1 January 1980) rather than at whatever the clock says.
const DOS_TIME = 0;
const DOS_DATE = 0x21;

const CRC_TABLE = (() => {
    const table = new Uint32Array(256);
    for (let byte = 0; byte < 256; byte += 1) {
        let value = byte;
        for (let bit = 0; bit < 8; bit += 1) {
            value = (value & 1) ? (0xedb88320 ^ (value >>> 1)) : (value >>> 1);
        }
        table[byte] = value >>> 0;
    }
    return table;
})();

function crc32(bytes) {
    let value = 0xffffffff;
    for (let index = 0; index < bytes.length; index += 1) {
        value = CRC_TABLE[(value ^ bytes[index]) & 0xff] ^ (value >>> 8);
    }
    return (value ^ 0xffffffff) >>> 0;
}

function record(size, fill) {
    const bytes = new Uint8Array(size);
    fill(new DataView(bytes.buffer));
    return bytes;
}

function joined(parts) {
    const total = parts.reduce((sum, part) => sum + part.length, 0);
    const out = new Uint8Array(total);
    let at = 0;
    for (const part of parts) {
        out.set(part, at);
        at += part.length;
    }
    return out;
}

// One zip archive holding `files`, each `{name, text}`, in the order they are given.
export function zipBytes(files) {
    const encoder = new TextEncoder();
    const entries = files.map((file) => {
        const name = encoder.encode(file.name);
        const data = encoder.encode(file.text);
        return {name, data, sum: crc32(data)};
    });

    const body = [];
    const directory = [];
    let offset = 0;
    for (const entry of entries) {
        const header = record(30, (view) => {
            view.setUint32(0, LOCAL_HEADER, true);
            view.setUint16(4, VERSION_NEEDED, true);
            view.setUint16(6, UTF8_NAMES, true);
            view.setUint16(8, STORED, true);
            view.setUint16(10, DOS_TIME, true);
            view.setUint16(12, DOS_DATE, true);
            view.setUint32(14, entry.sum, true);
            view.setUint32(18, entry.data.length, true);
            view.setUint32(22, entry.data.length, true);
            view.setUint16(26, entry.name.length, true);
            view.setUint16(28, 0, true);
        });
        directory.push(record(46, (view) => {
            view.setUint32(0, CENTRAL_HEADER, true);
            view.setUint16(4, VERSION_NEEDED, true);
            view.setUint16(6, VERSION_NEEDED, true);
            view.setUint16(8, UTF8_NAMES, true);
            view.setUint16(10, STORED, true);
            view.setUint16(12, DOS_TIME, true);
            view.setUint16(14, DOS_DATE, true);
            view.setUint32(16, entry.sum, true);
            view.setUint32(20, entry.data.length, true);
            view.setUint32(24, entry.data.length, true);
            view.setUint16(28, entry.name.length, true);
            view.setUint16(30, 0, true);          // extra field
            view.setUint16(32, 0, true);          // comment
            view.setUint16(34, 0, true);          // disk number
            view.setUint16(36, 0, true);          // internal attributes
            view.setUint32(38, 0, true);          // external attributes
            view.setUint32(42, offset, true);     // where its local header is
        }));
        directory.push(entry.name);
        body.push(header, entry.name, entry.data);
        offset += header.length + entry.name.length + entry.data.length;
    }

    const catalogue = joined(directory);
    const end = record(22, (view) => {
        view.setUint32(0, END_OF_CENTRAL, true);
        view.setUint16(4, 0, true);               // this disk
        view.setUint16(6, 0, true);               // the disk the directory starts on
        view.setUint16(8, entries.length, true);
        view.setUint16(10, entries.length, true);
        view.setUint32(12, catalogue.length, true);
        view.setUint32(16, offset, true);
        view.setUint16(20, 0, true);              // comment
    });
    return joined([...body, catalogue, end]);
}
