# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The contract generator through synqt_add_contract and repc, with a QtRO round trip.

cmake_minimum_required(VERSION 3.21)
project(synqt_m1_contract LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick RemoteObjects Test)
qt_standard_project_setup()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

enable_testing()

qt_add_executable(tst_m1 tst_m1.cpp)

# ROLE both: this target hosts the Source and acquires the Replica.
synqt_add_contract(tst_m1 ROLE both SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Todo.syn"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Catalog.syn"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Gated.syn"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Typed.syn"
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

# What a consumer can write into a published model, over a real connection.
qt_add_executable(tst_modelwrite tst_modelwrite.cpp)

synqt_add_contract(tst_modelwrite ROLE both SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Rows.syn"
)

target_link_libraries(tst_modelwrite PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME m1-modelwrite COMMAND tst_modelwrite)

# The QML type-resolution rules synqt/qmlrewrite.py relies on, checked against a real engine.
if(NOT TARGET SynQtConsumer)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/consumer"
                     "${CMAKE_BINARY_DIR}/SynQtConsumer")
endif()

qt_add_executable(tst_qmlrules tst_qmlrules.cpp)

target_link_libraries(tst_qmlrules PRIVATE
    SynQtConsumer
    Qt6::Core
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
)

# offscreen: the Item and Timer it loads are never shown, and a test target that needed a
# display would not run in CI.
add_test(NAME m1-qmlrules COMMAND tst_qmlrules -platform offscreen)

# Registering a contract's Sources alone must make `import SynQt` carry QtQuick. Its own
# process: the module-import registry is process-wide.
qt_add_executable(tst_qmlimport tst_qmlimport.cpp)

synqt_add_contract(tst_qmlimport ROLE source SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Todo.syn"
)

target_link_libraries(tst_qmlimport PRIVATE
    SynQtConsumer
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME m1-qmlimport COMMAND tst_qmlimport -platform offscreen)

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
