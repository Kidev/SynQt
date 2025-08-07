# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# FIX-3 acceptance: the stall storefront's edge-delivered pages and its real page seed. The
# web edge serves its Pages connect point; a native SynClient acts as the browser and
# fetches pages over the same authenticated wss link, so the seed is produced by the
# production per-connection Caller (WebEdge::hostConnection -> PagesEdgeSource), never a
# hand-built one. Native host kit; a localhost edge cert is generated at configure time.

cmake_minimum_required(VERSION 3.21)
project(synqt_fix3_stall LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Gui Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")

set(FIX3_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${FIX3_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "FIX-3 certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

# The example project the test drives its pages from, so the edge loads the very files the
# example ships (web/pages/*.qml and web/campaign-seed.qml).
set(STALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/stall")

qt_add_executable(tst_fix3 tst_fix3.cpp)

# Pages is the framework's own contract, generated once as part of SynQtService and exposed
# publicly from there (src/service/CMakeLists.txt); this test reaches "rep_pages_source.h"
# and PageResponse through that link rather than generating a second, colliding copy.
target_compile_definitions(tst_fix3 PRIVATE
    FIX3_CERT_DIR="${FIX3_CERTS}"
    FIX3_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
    FIX3_STALL_DIR="${STALL_DIR}"
)

target_link_libraries(tst_fix3 PRIVATE
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

add_test(NAME fix3 COMMAND tst_fix3)
