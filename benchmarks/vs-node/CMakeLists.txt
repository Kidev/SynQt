# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# SynQt's column of the Node.js comparison: the live path, over the real
# QtRemoteObjects-over-WebSockets stack a browser client reaches. The Node columns are plain
# .mjs under node/ and need no build. Not a ctest (a measurement run, not a pass/fail gate);
# build and run it through run-bench.sh.

cmake_minimum_required(VERSION 3.21)
project(synqt_bench_vs_node LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network WebSockets RemoteObjects)
qt_standard_project_setup()

# The framework client runtime library that owns WebSocketTransport: the harness measures
# through the adapter the real client uses, not through a QWebSocket of its own.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")

qt_add_executable(bench_live bench_live.cpp)

# One both-sided target: publisher and subscribers run in one process, on one clock, which
# is what keeps the propagation figure free of cross-process clock skew.
qt_add_repc_sources(bench_live "${CMAKE_CURRENT_SOURCE_DIR}/live.rep")
qt_add_repc_replicas(bench_live "${CMAKE_CURRENT_SOURCE_DIR}/live.rep")

target_include_directories(bench_live PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(bench_live PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::WebSockets
    Qt6::RemoteObjects
)

# The other direction: a caller asks and waits. This is the column the Next.js Server
# Function comparison is measured against, so it runs the same stack the live column does
# and differs only in which way the message goes first.
qt_add_executable(bench_call bench_call.cpp)

qt_add_repc_sources(bench_call "${CMAKE_CURRENT_SOURCE_DIR}/call.rep")
qt_add_repc_replicas(bench_call "${CMAKE_CURRENT_SOURCE_DIR}/call.rep")

target_include_directories(bench_call PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(bench_call PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::WebSockets
    Qt6::RemoteObjects
)
