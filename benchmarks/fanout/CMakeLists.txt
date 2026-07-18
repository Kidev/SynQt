# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The edge fan-out baseline (M5): how the arena's per-session publish() cost and propagation
# latency grow as one owner change reaches N consumers, and how interest management flattens the
# per-session payload. Native host kit; one host and N consumer nodes run in one process over
# loopback WebSockets, the same WebSocketTransport the real edge uses. Not a ctest (a measurement
# run, not a pass/fail gate); build and run it through run-bench.sh.

cmake_minimum_required(VERSION 3.21)
project(synqt_bench_fanout LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network WebSockets RemoteObjects)
qt_standard_project_setup()

# The framework client runtime library that owns WebSocketTransport.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")

qt_add_executable(bench_fanout bench_fanout.cpp)

# One both-sided target: the rep has no POD, so the source and replica headers coexist.
qt_add_repc_sources(bench_fanout "${CMAKE_CURRENT_SOURCE_DIR}/fanout.rep")
qt_add_repc_replicas(bench_fanout "${CMAKE_CURRENT_SOURCE_DIR}/fanout.rep")

target_include_directories(bench_fanout PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(bench_fanout PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::WebSockets
    Qt6::RemoteObjects
)
