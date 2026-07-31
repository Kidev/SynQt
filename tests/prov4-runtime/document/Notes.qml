// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick

// A document entity's owner-side Source. It calls the `Docs` helper only, passing the
// collection, the document and the filter as maps, so it names no engine: the same file
// works whether the provider is the embedded memory store or mongodb.
QtObject {
    Component.onCompleted: Docs.insert("notes", { "title": "written-at-source-creation" })

    function add(note) {
        return Docs.insert("notes", note);
    }

    function byAuthor(author) {
        return Docs.find("notes", { "author": author });
    }
}
