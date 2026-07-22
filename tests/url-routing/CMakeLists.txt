# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# URL routing acceptance: the client-side matcher and Router behavior (tst_routing), and
# the edge's application-shell fallback with its security headers (tst_spafallback).
# Native host kit; no browser needed.

cmake_minimum_required(VERSION 3.21)
project(synqt_url_routing LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network WebSockets Qml RemoteObjects Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")

enable_testing()

qt_add_executable(tst_routing tst_routing.cpp)
target_link_libraries(tst_routing PRIVATE SynQtClient Qt6::Test)
add_test(NAME routing COMMAND tst_routing)
