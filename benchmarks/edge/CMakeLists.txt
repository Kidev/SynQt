# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.21)
project(SynQtEdgeBench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core HttpServer Sql)
qt_standard_project_setup()

qt_add_executable(bench-edge bench_edge.cpp)

target_link_libraries(bench-edge PRIVATE
    Qt6::Core
    Qt6::HttpServer
    Qt6::Sql
)
