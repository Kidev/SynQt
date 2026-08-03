# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The QML test harness (`import SynQt.Test`), proven against a Source written the way an
# application writes one. It is the framework's own copy of what `synqt test` generates
# for a project, so a change that breaks an application's tests breaks this first.

cmake_minimum_required(VERSION 3.21)
project(synqt_entity_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick QuickTest RemoteObjects Test)
qt_standard_project_setup()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

enable_testing()

# Standalone (its own run-*.sh) or inside the whole-tree build: either way the runtime
# libraries are added once, by whichever scope gets here first.
if(NOT TARGET SynQtTesting)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/transport" synqt_transport)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/consumer" synqt_consumer)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/providers" synqt_providers)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" synqt_client)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" synqt_service)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/testing" synqt_testing)
endif()

qt_add_executable(tst_entitytest tst_entitytest.cpp)

# ROLE source: the harness drives an owner's Source, so only that half is generated. A
# test that could also acquire a Replica would be testing the transport, not the slot.
synqt_add_contract(tst_entitytest ROLE source SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Ledger.syn"
)

# Qt Quick Test finds the tst_*.qml files by directory at run time.
target_compile_definitions(tst_entitytest PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
)

target_link_libraries(tst_entitytest PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickTest
    Qt6::RemoteObjects
    Qt6::Test
    SynQtTesting
)

add_test(NAME entity-test COMMAND tst_entitytest -platform offscreen)
