// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// A hook an author got wrong the way the framework most expects: seedFor is declared, but a
// parameter is annotated. The edge calls seedFor with untyped (QVariant) arguments, so a
// typed parameter changes the method signature and the call can never bind to it. It must be
// caught when the hook is built, loudly and once, and the page delivered with no seed rather
// than the edge failing (and logging) every request the browser makes.
PageSeed {
    function seedFor(route: string, parameters, caller) {
        return {
            "headline": route
        };
    }
}
