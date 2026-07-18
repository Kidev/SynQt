# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M1 acceptance: the contract generator, driven through the synqt_add_contract CMake
# helper, feeding repc, with an in-process QtRO round trip proving the generated
# layer compiles and behaves. Built with the native host Qt kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_m1_contract LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml RemoteObjects Test)
qt_standard_project_setup()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

enable_testing()

qt_add_executable(tst_m1 tst_m1.cpp)

# ROLE both: this single test target hosts the Source and acquires the Replica, so
# it generates and links both sides. A real client entity would use ROLE replica and
# a service entity ROLE source, keeping the Source helper out of the client.
synqt_add_contract(tst_m1 ROLE both SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Todo.syn"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Catalog.syn"
)

target_compile_definitions(tst_m1 PRIVATE
    M1_TODO_REP="${CMAKE_CURRENT_BINARY_DIR}/synqt_generated/tst_m1/todo.rep"
    M1_CATALOG_REP="${CMAKE_CURRENT_BINARY_DIR}/synqt_generated/tst_m1/catalog.rep"
)

target_link_libraries(tst_m1 PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME m1 COMMAND tst_m1)

# Owner (ROLE source): gets the Source helpers only. Proves a service entity builds
# with no Replica code.
qt_add_executable(m1_owner owner_main.cpp)
synqt_add_contract(m1_owner ROLE source SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Todo.syn"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Catalog.syn"
)
target_link_libraries(m1_owner PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::RemoteObjects)

# Consumer (ROLE replica): gets the Replica registrations only, and links WITHOUT
# Qt Gui (which the Source helper needs); the Source helper never reaches the client.
qt_add_executable(m1_client client_main.cpp)
synqt_add_contract(m1_client ROLE replica SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Todo.syn"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Catalog.syn"
)
target_link_libraries(m1_client PRIVATE
    Qt6::Core Qt6::Qml Qt6::RemoteObjects)
