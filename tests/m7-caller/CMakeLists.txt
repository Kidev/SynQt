# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# SessionManager, Caller, connect-point scope gating and per-peer
# authorization, end to end on the three-entity todo. Native host kit; throwaway
# certificates are generated at configure time.

cmake_minimum_required(VERSION 3.21)
project(synqt_m7_caller LANGUAGES CXX)

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
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(M7_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${M7_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "M7 certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_m7 tst_m7.cpp)

# Both entities host Sources from their QML (TodoSource on the edge, ItemsSource on the
# database); every consumer uses dynamic replicas, so no replica headers are generated.
synqt_add_contract(tst_m7 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Todo.syn")
# Items: the edge consumes it (ROLE both), and every call carries the session (FORWARDS_SESSION).
synqt_add_contract(tst_m7 ROLE both FORWARDS_SESSION
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Items.syn")
# Draft holds state in the Source and nowhere else, which is what lets a test see how
# many Sources one connect point minted.
synqt_add_contract(tst_m7 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Draft.syn")
# A front (`behind:`): Gated is what the browser acquires, Backoffice the slice behind it.
synqt_add_contract(tst_m7 ROLE source SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Gated.syn"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Backoffice.syn")

target_compile_definitions(tst_m7 PRIVATE
    M7_CERT_DIR="${M7_CERTS}"
    M7_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_m7 PRIVATE
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

add_test(NAME m7 COMMAND tst_m7)

# SessionManager and Caller unit cases: expiry, rotation, hierarchical and set-based scopes.
qt_add_executable(tst_sessions tst_sessions.cpp)

target_link_libraries(tst_sessions PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Test
)

add_test(NAME sessions COMMAND tst_sessions)

# The `<scope>` gate on a member: what a per-caller mirror publishes.
qt_add_executable(tst_memberscope tst_memberscope.cpp)

synqt_add_contract(tst_memberscope ROLE source SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Gated.syn")

target_link_libraries(tst_memberscope PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME memberscope COMMAND tst_memberscope)

# A front's relay: one caller's view of the entity serving their scope.
qt_add_executable(tst_front tst_front.cpp)

synqt_add_contract(tst_front ROLE source SYN
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Gated.syn"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/Backoffice.syn")

target_link_libraries(tst_front PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME front COMMAND tst_front)
