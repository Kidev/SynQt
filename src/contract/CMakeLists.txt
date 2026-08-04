# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The runtime support the generated contract layer needs, for whoever owns a connect
# point. Today that is SourceModel, the published model a consumer cannot write into.
#
# It is a library rather than a header the generator emits into every target because the
# rule is a security boundary: one definition, linked by synqt_add_contract for every
# owner-side target, and no way for a target to end up with a different one. Qt Gui is
# here because QStandardItemModel is; the client never links this.

# GLOBAL: see the note in src/service/CMakeLists.txt. Qt6::Gui is public on this target,
# so it has to resolve in whatever scope the consuming executable is finalized in.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui GLOBAL)

qt_add_library(SynQtContract STATIC
    sourcemodel.cpp
    sourcemodel.h
)

set_target_properties(SynQtContract PROPERTIES AUTOMOC ON)

target_include_directories(SynQtContract PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SynQtContract PUBLIC
    Qt6::Core
    Qt6::Gui
)
