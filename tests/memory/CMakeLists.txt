# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Memory acceptance: what a repeated workload leaves behind. Native host kit, real TLS on
# the edge (a throwaway localhost server certificate generated at configure time into the
# build tree, never committed).

cmake_minimum_required(VERSION 3.21)
project(synqt_memory LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtService)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(MEMORY_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${MEMORY_CERTS}"
    RESULT_VARIABLE _cert_rc
    ERROR_VARIABLE _cert_err
)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "memory suite certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_memory tst_memory.cpp)

# ROLE source: both owners here (the edge and the service entity) host the Probe Source
# from QML; the consumer side acquires it dynamically, so no replica header is needed.
synqt_add_contract(tst_memory ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Probe.syn")

target_compile_definitions(tst_memory PRIVATE
    MEMORY_CERT_DIR="${MEMORY_CERTS}"
    MEMORY_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_memory PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)

add_test(NAME memory COMMAND tst_memory)
