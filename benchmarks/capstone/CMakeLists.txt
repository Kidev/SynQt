# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The capstone load test: the multiplayer arena end to end, swept over player count, to find
# where a single-edge version-1 deployment saturates. One process runs the server-authoritative
# arena simulation, one per-session Source per player, and N headless player nodes over the real
# WebSocketTransport on loopback. Native host kit. Not a ctest (a measurement run, not a pass/fail
# gate); build and run it through run-bench.sh, which writes a JSON baseline under
# benchmarks/results/.

cmake_minimum_required(VERSION 3.21)
project(synqt_bench_capstone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network WebSockets RemoteObjects)
qt_standard_project_setup()

# The framework client runtime library that owns WebSocketTransport.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")

qt_add_executable(bench_capstone bench_capstone.cpp)

# One both-sided target: the rep has no POD, so the source and replica headers coexist.
qt_add_repc_sources(bench_capstone "${CMAKE_CURRENT_SOURCE_DIR}/capstone.rep")
qt_add_repc_replicas(bench_capstone "${CMAKE_CURRENT_SOURCE_DIR}/capstone.rep")

target_include_directories(bench_capstone PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(bench_capstone PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::WebSockets
    Qt6::RemoteObjects
)
