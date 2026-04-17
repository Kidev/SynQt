// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MODULEIMPORTS_H
#define SYNQT_MODULEIMPORTS_H

namespace SynQt {

/// Makes `import SynQt` bring QtQuick in with it, so a file that imports the framework can
/// write `Item`, `Timer` or `Component.onCompleted` without a second import line.
///
/// This is the C++ spelling of a qmldir `import` declaration, whose documented effect is that
/// "the types from the other module are made available in the same type namespace as this
/// module is imported into" (Module Definition qmldir Files). SynQt registers its types
/// imperatively, so the declaration is made here. It carries no version: `auto` would ask for
/// QtQuick at SynQt's own version, 1.0, which does not exist.
///
/// It shadows nothing. An explicit `import QtQuick` in a file resolves as before, and a type a
/// file declares keeps its own name.
///
/// Call this before an engine loads anything. Registration is global to the QML type system,
/// and the call is idempotent.
///
/// Every generated contract registration makes this call first, so a caller needs it only when
/// there is no contract to register, such as a client with no consumed connect points.
void registerModuleImports();

} // namespace SynQt

#endif // SYNQT_MODULEIMPORTS_H
