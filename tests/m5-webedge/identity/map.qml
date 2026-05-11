// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A mapping hook, as a project writes one, for the tests about what the development picker
// makes of a named identity. A real one has `import SynQt` and an `IdentityMapping` root;
// this suite's engine registers no SynQt types, and the edge reads the hook through its
// metaobject either way, so the root here is a plain QtObject and the function is the same
// function. tests/m8-auth covers the real shape.
import QtQml

QtObject {
    function scopeFor(identity): int {
        // One person this project will not have. 9 is past the end of Scope.Value, which is
        // how a hook refuses somebody: there is no member meaning "no".
        if (identity.email === "banned@example.com") {
            return 9;
        }
        return Scope.Value.User;
    }
}
