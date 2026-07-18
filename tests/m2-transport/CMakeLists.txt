# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M2 acceptance: the client's WebSocketTransport adapter, proven by acquiring a host
# Source through it over a real local plaintext WebSocket. Native host kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_m2_transport LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network WebSockets RemoteObjects Test)
qt_standard_project_setup()

# The framework client runtime library that owns WebSocketTransport.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")

enable_testing()

qt_add_executable(tst_m2 tst_m2.cpp)

# A single both-sided test target; the rep has no POD, so separate source and replica
# headers coexist in one target without a duplicate-symbol clash.
qt_add_repc_sources(tst_m2 "${CMAKE_CURRENT_SOURCE_DIR}/m2.rep")
qt_add_repc_replicas(tst_m2 "${CMAKE_CURRENT_SOURCE_DIR}/m2.rep")

target_include_directories(tst_m2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(tst_m2 PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::WebSockets
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME m2 COMMAND tst_m2)
