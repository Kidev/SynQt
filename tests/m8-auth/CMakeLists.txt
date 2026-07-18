# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M8 acceptance: the edge login/callback flow (Authorization Code + PKCE + framework
# state + edge-held secret), identity normalization, the scope-mapping hook, the httpOnly
# session cookie, and the dev-stub gate. Driven end to end against the in-process dev stub
# provider. Native host kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_m8_auth LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Gui Network NetworkAuth Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(M8_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${M8_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "M8 certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_m8 tst_m8.cpp)
# The auth entity hosts the Session Source; the edges consume it as a dynamic Replica over
# the mesh, so only the Source side is generated.
synqt_add_contract(tst_m8 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Session.syn")
# The auth entity hosts the Identity Source (the centralized OAuth engine); the edges
# consume it as a dynamic Replica over the mesh, so only the Source side is generated.
synqt_add_contract(tst_m8 ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Identity.syn")
target_compile_definitions(tst_m8 PRIVATE
    M8_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
    M8_CERT_DIR="${M8_CERTS}")
target_link_libraries(tst_m8 PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::NetworkAuth
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)

add_test(NAME m8 COMMAND tst_m8)
