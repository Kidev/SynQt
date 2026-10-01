# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The mesh transports (mutual TLS by default, opt-in local socket).
# Native host kit. Throwaway test certificates are generated at configure time into
# the build tree (never committed).

cmake_minimum_required(VERSION 3.21)
project(synqt_mesh LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network RemoteObjects Test)
qt_standard_project_setup()

# The framework service runtime library that owns the mesh transport.
# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtService)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
endif()

# Generate the test certificates now (project CA + entity certs + a foreign CA).
set(MESH_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-certs.sh" "${MESH_CERTS}"
    RESULT_VARIABLE _certs_rc
    ERROR_VARIABLE _certs_err
)
if(NOT _certs_rc EQUAL 0)
    message(FATAL_ERROR "Certificate generation failed:\n${_certs_err}")
endif()

enable_testing()

qt_add_executable(tst_mesh tst_mesh.cpp)

# No POD in the rep, so source and replica headers coexist in this both-sided target.
qt_add_repc_sources(tst_mesh "${CMAKE_CURRENT_SOURCE_DIR}/mesh.rep")
qt_add_repc_replicas(tst_mesh "${CMAKE_CURRENT_SOURCE_DIR}/mesh.rep")

target_include_directories(tst_mesh PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(tst_mesh PRIVATE
    MESH_CERT_DIR="${MESH_CERTS}"
    # A crash-safe progress trace: a process that dies before QtTest writes any output still
    # leaves this file (see the note above main() in tst_mesh.cpp), and the run script dumps it
    # after the run to show the last step reached. Absolute path, so the test's working
    # directory does not matter.
    MESH_TRACE_LOG="${CMAKE_CURRENT_BINARY_DIR}/mesh-trace.log"
)

target_link_libraries(tst_mesh PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME mesh COMMAND tst_mesh)
