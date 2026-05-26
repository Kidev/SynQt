# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The monitoring pipeline benchmark: what recording an event costs the entity that
# records it. Native host kit; links the framework service runtime for the real Tracer
# and EventRing.

cmake_minimum_required(VERSION 3.21)
project(synqt_bench_monitor LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Qml RemoteObjects WebSockets)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")

qt_add_executable(bench_pipeline bench_pipeline.cpp)

target_link_libraries(bench_pipeline PRIVATE SynQtService Qt6::Core)
