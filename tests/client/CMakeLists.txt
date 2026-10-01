# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The client runtime, natively (which is also the desktop runtime): the counter runs against
# a real web edge, two clients sync, state transitions, reconnect, route guard.
# Native host kit. A throwaway localhost server cert is generated at configure time.

cmake_minimum_required(VERSION 3.21)
project(synqt_client LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

# The edge (SynQtEdge) and the client runtime (SynQtClient); both share SynQtTransport.
# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtEdge)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/edge" "${CMAKE_BINARY_DIR}/SynQtEdge")
endif()
if(NOT TARGET SynQtClient)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(CLIENT_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${CLIENT_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "Certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_client tst_client.cpp)

# ROLE source: the edge hosts the Counter Source (from web/Counter.qml); the clients use
# dynamic replicas, so no replica header is needed.
synqt_add_contract(tst_client ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Counter.syn")

target_compile_definitions(tst_client PRIVATE
    CLIENT_CERT_DIR="${CLIENT_CERTS}"
    CLIENT_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_client PRIVATE
    SynQtEdge
    SynQtClient
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)

add_test(NAME client COMMAND tst_client)
