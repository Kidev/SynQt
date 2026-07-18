# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# FIX-2 acceptance: the multiplayer tutorial's movement authority and guest-list gate. The
# web edge owns one authoritative arena (a World simulated once, injected into each
# per_session Arena Source by name) and integrates every blob itself from an aim point;
# `scope: player` on the connect point means an under-scoped session never acquires the
# arena at all. Native host kit; a localhost edge cert is generated at configure time.

cmake_minimum_required(VERSION 3.21)
project(synqt_fix2_arena LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Gui Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(FIX2_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${FIX2_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "FIX-2 certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_fix2 tst_fix2.cpp)

# The web edge hosts ArenaSource from its QML; the client uses a dynamic replica.
synqt_add_contract(tst_fix2 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Arena.syn")

target_compile_definitions(tst_fix2 PRIVATE
    FIX2_CERT_DIR="${FIX2_CERTS}"
    FIX2_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_fix2 PRIVATE
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

add_test(NAME fix2 COMMAND tst_fix2)
