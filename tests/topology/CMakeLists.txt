# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The entity runtime and topology (owned/consumed connect points,
# deny by default). Native host kit. Throwaway mesh certificates are generated at
# configure time into the build tree (never committed).

cmake_minimum_required(VERSION 3.21)
project(synqt_topology LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Qml RemoteObjects Test)
qt_standard_project_setup()

# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtService)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(TOPOLOGY_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-certs.sh" "${TOPOLOGY_CERTS}"
    RESULT_VARIABLE _certs_rc
    ERROR_VARIABLE _certs_err
)
if(NOT _certs_rc EQUAL 0)
    message(FATAL_ERROR "Certificate generation failed:\n${_certs_err}")
endif()

enable_testing()

qt_add_executable(tst_topology tst_topology.cpp)

# ROLE source: this test process hosts the owner Source (from QML). Consumers use a
# dynamic replica (acquireDynamic), so no replica header is needed.
synqt_add_contract(tst_topology ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Thing.syn")

target_compile_definitions(tst_topology PRIVATE
    TOPOLOGY_CERT_DIR="${TOPOLOGY_CERTS}"
    TOPOLOGY_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_topology PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME topology COMMAND tst_topology)
