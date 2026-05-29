# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.21)
project(SynQtEdgeBench LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core HttpServer Sql)
qt_standard_project_setup()

# The transport library, for one function: the event-dispatcher choice every generated entity
# makes before its application exists. This harness holds a couple of hundred connections, so
# running it on a dispatcher the framework does not ship would measure the wrong program.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/transport" "${CMAKE_BINARY_DIR}/SynQtTransport")

qt_add_executable(bench-edge bench_edge.cpp)

target_link_libraries(bench-edge PRIVATE
    SynQtTransport
    Qt6::Core
    Qt6::HttpServer
    Qt6::Sql
)
