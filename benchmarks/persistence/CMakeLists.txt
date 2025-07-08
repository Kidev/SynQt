# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The persistence + cache benchmark (M9): the SqliteProvider (WAL, busy timeout, serialized
# writer) and the MemoryCacheProvider (bounded LRU), measured through the real provider
# classes. Native host kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_bench_persistence LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Sql Network Qml)
find_package(Threads REQUIRED)
qt_standard_project_setup()

# The framework providers library (SqliteProvider, MemoryCacheProvider, ...).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/providers" "${CMAKE_BINARY_DIR}/SynQtProviders")

qt_add_executable(bench_persistence bench_persistence.cpp)

target_link_libraries(bench_persistence PRIVATE
    SynQtProviders
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Qt6::Network
    Qt6::Qml
    Threads::Threads
)
