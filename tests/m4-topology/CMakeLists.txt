# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M4 acceptance: the entity runtime and topology (owned/consumed connect points,
# deny by default). Native host kit. Throwaway mesh certificates are generated at
# configure time into the build tree (never committed).

cmake_minimum_required(VERSION 3.21)
project(synqt_m4_topology LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network Qml RemoteObjects Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(M4_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-certs.sh" "${M4_CERTS}"
    RESULT_VARIABLE _certs_rc
    ERROR_VARIABLE _certs_err
)
if(NOT _certs_rc EQUAL 0)
    message(FATAL_ERROR "M4 certificate generation failed:\n${_certs_err}")
endif()

enable_testing()

qt_add_executable(tst_m4 tst_m4.cpp)

# ROLE source: this test process hosts the owner Source (from QML). Consumers use a
# dynamic replica (acquireDynamic), so no replica header is needed.
synqt_add_contract(tst_m4 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Thing.syn")

target_compile_definitions(tst_m4 PRIVATE
    M4_CERT_DIR="${M4_CERTS}"
    M4_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_m4 PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME m4 COMMAND tst_m4)
