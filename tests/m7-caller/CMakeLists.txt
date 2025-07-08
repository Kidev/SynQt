# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M7 acceptance: SessionManager, the Caller accessor in every slot, connect-point scope
# gating, and per-peer authorization, proven end to end on the three-entity todo. The
# database (owner of `items`) authorizes the calling entity; the edge (owner of `todo`,
# per_session) authorizes the user; the client is a browser. Native host kit; throwaway
# CA + entity certs and a localhost edge cert are generated at configure time.

cmake_minimum_required(VERSION 3.21)
project(synqt_m7_caller LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Gui Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
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
synqt_add_contract(tst_m7 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Items.syn")

target_compile_definitions(tst_m7 PRIVATE
    M7_CERT_DIR="${M7_CERTS}"
    M7_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_m7 PRIVATE
    SynQtService
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
