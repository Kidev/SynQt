# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M5 acceptance: the web edge, over real TLS. Native host kit. A throwaway localhost
# server certificate is generated at configure time into the build tree (never committed).

cmake_minimum_required(VERSION 3.21)
project(synqt_m5_webedge LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtEdge)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/edge" "${CMAKE_BINARY_DIR}/SynQtEdge")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(M5_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${M5_CERTS}"
    RESULT_VARIABLE _cert_rc
    ERROR_VARIABLE _cert_err
)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "M5 certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_m5 tst_m5.cpp)

# ROLE source: the edge hosts the greeting Source (from QML); the browser stand-in
# acquires it dynamically, so no replica header is needed.
synqt_add_contract(tst_m5 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Greeting.syn")

target_compile_definitions(tst_m5 PRIVATE
    M5_CERT_DIR="${M5_CERTS}"
    M5_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_m5 PRIVATE
    SynQtEdge
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)

add_test(NAME m5 COMMAND tst_m5)

# The client-address resolver on its own. It needs no server, no certificate and no
# contract: it is a pure function of the peer, the header and the configured list, and
# that is exactly why it is testable to the corner cases an end-to-end upgrade cannot
# reach (a forged hop, a trusted hop, a malformed entry).
#
# It links SynQtService, which is where the class lives, and not SynQtEdge: the browser
# side of an edge and an entity's inbound API surface both resolve a client address, and
# only one of those two may link Qt HTTP Server. Building the test against the narrower
# library is what keeps that true.
qt_add_executable(tst_clientaddress tst_clientaddress.cpp)

target_link_libraries(tst_clientaddress PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::Test
)

add_test(NAME clientaddress COMMAND tst_clientaddress)

# The rate-window table on its own. Three gates share it, and its interesting case is what
# a table does when it fills up, which an end-to-end test cannot reach without arriving from
# four thousand addresses. Same reasoning as tst_clientaddress above.
qt_add_executable(tst_ratewindow tst_ratewindow.cpp)

target_link_libraries(tst_ratewindow PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Test
)

add_test(NAME ratewindow COMMAND tst_ratewindow)
