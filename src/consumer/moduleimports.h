// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MODULEIMPORTS_H
#define SYNQT_MODULEIMPORTS_H

namespace SynQt {

/// Makes `import SynQt` bring QtQuick in with it, so a file that imports the
/// framework can write `Item`, `Timer` or `Component.onCompleted` without a
/// second import line above the first.
///
/// Every SynQt file needs both. The framework's own types are useless on their
/// own. An entity's file is a QtObject or a Source with QML bindings in it, and
/// a client's is a window. Writing `import QtQuick` above `import SynQt` in
/// every file was therefore a line that carried no decision, and leaving it out
/// failed at load time with "Item is not a type", which names the symbol and
/// not the missing import.
///
/// This is the C++ spelling of a qmldir `import` declaration, whose documented
/// effect is that "the types from the other module are made available in the
/// same type namespace as this module is imported into" (Module Definition
/// qmldir Files). SynQt registers its types imperatively rather than through a
/// qmldir, so the declaration is made here instead. Note the deliberate absence
/// of a version: `auto` would ask for QtQuick at SynQt's own version, 1.0,
/// which does not exist and fails the import outright.
///
/// It does not shadow anything. A file that still writes `import QtQuick`
/// itself resolves exactly as before, and a file that imports SynQt and then
/// declares a type of its own keeps its own: an explicit import in the document
/// wins over one the module brought along.
///
/// Call this before an engine loads anything, once per process. Registration is
/// global to the QML type system rather than per engine, and repeating it is
/// harmless.
void registerModuleImports();

} // namespace SynQt

#endif // SYNQT_MODULEIMPORTS_H
