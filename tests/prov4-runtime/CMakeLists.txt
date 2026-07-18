# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# PROV-4 acceptance: EntityRuntime injects a blueprint's backend helper (Db/Cache/Http/Jobs)
# into every owned Source from the entity's provider config, with no manual wiring. Native
# host kit; a local-socket connect point keeps the owner cert-free (this proves injection,
# not the mesh, which M3/M4 already cover).

cmake_minimum_required(VERSION 3.21)
project(synqt_prov4_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Sql Network Qml RemoteObjects Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")

enable_testing()

qt_add_executable(tst_prov4 tst_prov4.cpp)
target_compile_definitions(tst_prov4 PRIVATE PROV4_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tst_prov4 PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME prov4 COMMAND tst_prov4)
