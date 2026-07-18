# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The mesh-transport benchmark (M3): mutual TLS on loopback vs the opt-in local socket,
# through the framework's SynQt::MeshServer / MeshClient. Native host kit. Throwaway
# certificates are generated at configure time into the build tree (never committed).

cmake_minimum_required(VERSION 3.21)
project(synqt_bench_mesh LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network RemoteObjects)
qt_standard_project_setup()

# The framework service runtime library that owns the mesh transport.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")

# Generate the benchmark certificates now (project CA + owner/consumer entity certs).
set(MESH_BENCH_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-certs.sh" "${MESH_BENCH_CERTS}"
    RESULT_VARIABLE _certs_rc
    ERROR_VARIABLE _certs_err
)
if(NOT _certs_rc EQUAL 0)
    message(FATAL_ERROR "mesh benchmark certificate generation failed:\n${_certs_err}")
endif()

qt_add_executable(bench_mesh bench_mesh.cpp)

# No POD in the rep, so source and replica headers coexist in this both-sided target.
qt_add_repc_sources(bench_mesh "${CMAKE_CURRENT_SOURCE_DIR}/bench_mesh.rep")
qt_add_repc_replicas(bench_mesh "${CMAKE_CURRENT_SOURCE_DIR}/bench_mesh.rep")

target_include_directories(bench_mesh PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(bench_mesh PRIVATE MESH_BENCH_CERT_DIR="${MESH_BENCH_CERTS}")

target_link_libraries(bench_mesh PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
)
