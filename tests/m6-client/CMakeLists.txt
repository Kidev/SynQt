# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M6 acceptance (functional core, native = the desktop runtime): the counter runs
# against a real web edge, two clients sync, state transitions, reconnect, route guard.
# Native host kit. A throwaway localhost server cert is generated at configure time.

cmake_minimum_required(VERSION 3.21)
project(synqt_m6_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

# The edge (SynQtService) and the client runtime (SynQtClient); both share SynQtTransport.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(M6_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${M6_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "M6 certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_m6 tst_m6.cpp)

# ROLE source: the edge hosts the Counter Source (from web/Counter.qml); the clients use
# dynamic replicas, so no replica header is needed.
synqt_add_contract(tst_m6 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Counter.syn")

target_compile_definitions(tst_m6 PRIVATE
    M6_CERT_DIR="${M6_CERTS}"
    M6_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_m6 PRIVATE
    SynQtService
    SynQtClient
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)

add_test(NAME m6 COMMAND tst_m6)
