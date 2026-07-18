# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M3 acceptance: the mesh transports (mutual TLS by default, opt-in local socket).
# Native host kit. Throwaway test certificates are generated at configure time into
# the build tree (never committed).

cmake_minimum_required(VERSION 3.21)
project(synqt_m3_mesh LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network RemoteObjects Test)
qt_standard_project_setup()

# The framework service runtime library that owns the mesh transport.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")

# Generate the test certificates now (project CA + entity certs + a foreign CA).
set(M3_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-certs.sh" "${M3_CERTS}"
    RESULT_VARIABLE _certs_rc
    ERROR_VARIABLE _certs_err
)
if(NOT _certs_rc EQUAL 0)
    message(FATAL_ERROR "M3 certificate generation failed:\n${_certs_err}")
endif()

enable_testing()

qt_add_executable(tst_m3 tst_m3.cpp)

# No POD in the rep, so source and replica headers coexist in this both-sided target.
qt_add_repc_sources(tst_m3 "${CMAKE_CURRENT_SOURCE_DIR}/mesh.rep")
qt_add_repc_replicas(tst_m3 "${CMAKE_CURRENT_SOURCE_DIR}/mesh.rep")

target_include_directories(tst_m3 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(tst_m3 PRIVATE
    M3_CERT_DIR="${M3_CERTS}"
    # A crash-safe progress trace: the Windows-only failure dies before writing a single line of
    # QtTest output (see the note above main() in tst_m3.cpp), so the run script dumps this file
    # after the run to reveal the last step the process reached. Absolute path, so the test's
    # working directory does not matter.
    M3_TRACE_LOG="${CMAKE_CURRENT_BINARY_DIR}/m3-trace.log"
)

target_link_libraries(tst_m3 PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME m3 COMMAND tst_m3)
