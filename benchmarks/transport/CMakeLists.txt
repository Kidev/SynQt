# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The client-to-edge transport baseline (M0 path: QtRO over QtWebSockets). Native host kit;
# host and client run in one process over a loopback WebSocket, the same WebSocketTransport
# the real client uses. Not a ctest (it is a measurement run, not a pass/fail gate); build
# and run it through run-bench.sh.

cmake_minimum_required(VERSION 3.21)
project(synqt_bench_transport LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network WebSockets RemoteObjects)
qt_standard_project_setup()

# The framework client runtime library that owns WebSocketTransport.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")

qt_add_executable(bench_transport bench_transport.cpp)

# One both-sided target: the rep has no POD, so the source and replica headers coexist.
qt_add_repc_sources(bench_transport "${CMAKE_CURRENT_SOURCE_DIR}/bench.rep")
qt_add_repc_replicas(bench_transport "${CMAKE_CURRENT_SOURCE_DIR}/bench.rep")

target_include_directories(bench_transport PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(bench_transport PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::WebSockets
    Qt6::RemoteObjects
)
