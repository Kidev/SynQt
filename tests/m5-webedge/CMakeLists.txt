# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M5 acceptance: the web edge, over real TLS. Native host kit. A throwaway localhost
# server certificate is generated at configure time into the build tree (never committed).

cmake_minimum_required(VERSION 3.21)
project(synqt_m5_webedge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
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
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)

add_test(NAME m5 COMMAND tst_m5)
