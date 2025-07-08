# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The sessions benchmark (M7): SessionManager lookup and Caller.hasScope cost as the live
# session count grows. Native host kit. Links the framework service runtime for the real
# SessionManager and Caller.

cmake_minimum_required(VERSION 3.21)
project(synqt_bench_sessions LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Gui Network NetworkAuth Qml RemoteObjects WebSockets HttpServer)
qt_standard_project_setup()

# The framework service runtime library that owns SessionManager and Caller (PUBLIC Qt deps,
# so linking it is enough).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")

qt_add_executable(bench_sessions bench_sessions.cpp)

target_link_libraries(bench_sessions PRIVATE SynQtService Qt6::Core)
