// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick

// A document entity's own file: the entity itself, alive for as long as the entity runs.
// It calls the `Docs` helper only, passing the collection, the document and the filter as
// maps, so it names no engine. The same file works whether the provider is the embedded
// memory store or mongodb.
pragma Singleton

QtObject {
    Component.onCompleted: Docs.insert("notes", { "title": "written-at-source-creation" })

    function add(note) {
        return Docs.insert("notes", note);
    }

    function byAuthor(author) {
        return Docs.find("notes", { "author": author });
    }
}
