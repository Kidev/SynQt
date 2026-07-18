# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M9 acceptance: the family interfaces and bundled providers. Persistence (sqlite: store,
# query, persist across restart, injection inertness, no deadlock; the Source calls only
# the Db interface; postgres refuses plaintext in release), cache (bounded eviction),
# document round-trip, and the gateway/jobs helpers. Native host kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_m9_providers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Sql Network Qml Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/providers" "${CMAKE_BINARY_DIR}/SynQtProviders")

enable_testing()

qt_add_executable(tst_m9 tst_m9.cpp)
target_compile_definitions(tst_m9 PRIVATE M9_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tst_m9 PRIVATE
    SynQtProviders
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Qt6::Network
    Qt6::Qml
    Qt6::Test
)

add_test(NAME m9 COMMAND tst_m9)
