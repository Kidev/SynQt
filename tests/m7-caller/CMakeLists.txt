# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M7 acceptance: SessionManager, the Caller accessor in every slot, connect-point scope
# gating, and per-peer authorization, proven end to end on the three-entity todo. The
# database (owner of `items`) authorizes the calling entity; the edge (owner of `todo`,
# a Source per caller) authorizes the user; the client is a browser. Native host kit; throwaway
# CA + entity certs and a localhost edge cert are generated at configure time.

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
# Items is the mesh half: the database owns it and the edge consumes it, so the edge needs
# the consumer facade (ROLE both) and every call on it carries the session the edge is
# acting for (FORWARDS_SESSION), which is how the database knows who a write is really for.
synqt_add_contract(tst_m7 ROLE both FORWARDS_SESSION
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Items.syn")
# Draft holds state in the Source and nowhere else, which is what lets a test see how
# many Sources one connect point minted.
synqt_add_contract(tst_m7 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Draft.syn")

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

# The SessionManager and Caller unit cases from the test plan (TTL expiry and its purge,
# credential rotation, hierarchical versus set-based scope checks). No entities, no
# transport: the two classes are the subject, and tst_m7 above answers the end-to-end
# question they serve.
qt_add_executable(tst_sessions tst_sessions.cpp)

target_link_libraries(tst_sessions PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Test
)

add_test(NAME sessions COMMAND tst_sessions)

# The `<scope>` gate on a member, over the shape the runtime builds: one shared Source and
# a mirror per caller. No transport and no QML, because the question is what a mirror
# publishes, and that is decided before anything is on a wire.
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

# A front's relay: the Source a browser acquires is one caller's view of the entity serving
# their scope. Two contracts, because that is the shape (the front's surface and the slice
# one entity behind it answers), and no transport, because the relay works by name and has
# no opinion about what is on the other side.
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
