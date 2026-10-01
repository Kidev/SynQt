# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The stall storefront's edge-delivered pages and page seed, fetched by a
# native SynClient over the authenticated wss link. Native host kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_stall LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core Gui Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtEdge)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/edge" "${CMAKE_BINARY_DIR}/SynQtEdge")
endif()
if(NOT TARGET SynQtClient)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
endif()

set(STALL_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${STALL_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "stall: certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

# The example project the test drives its pages from, so the edge loads the very files the
# example ships (web/pages/*.qml and web/campaign-seed.qml).
set(STALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/stall")

qt_add_executable(tst_stall tst_stall.cpp)

# Pages comes from SynQtEdge, never a second generated copy.
target_compile_definitions(tst_stall PRIVATE
    STALL_CERT_DIR="${STALL_CERTS}"
    STALL_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
    STALL_DIR="${STALL_DIR}"
)

target_link_libraries(tst_stall PRIVATE
    SynQtEdge
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)

add_test(NAME stall COMMAND tst_stall)

# The client half runs in its own process: both sides would define the Pages record.
qt_add_executable(stall_edge stall_edge.cpp)
target_compile_definitions(stall_edge PRIVATE
    STALL_CERT_DIR="${STALL_CERTS}"
    STALL_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
    STALL_DIR="${STALL_DIR}"
)
target_link_libraries(stall_edge PRIVATE SynQtEdge Qt6::Core Qt6::Qml)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")
qt_add_executable(tst_stall_client tst_stall_client.cpp)
synqt_add_contract(tst_stall_client ROLE replica
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/../../src/edge/contracts/Pages.syn")
target_compile_definitions(tst_stall_client PRIVATE
    STALL_CERT_DIR="${STALL_CERTS}"
    STALL_EDGE_BINARY="$<TARGET_FILE:stall_edge>"
)
target_link_libraries(tst_stall_client PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::Test
)
add_dependencies(tst_stall_client stall_edge)

add_test(NAME stall-client COMMAND tst_stall_client)

