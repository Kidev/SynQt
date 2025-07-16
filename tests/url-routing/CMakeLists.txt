# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# URL routing acceptance: the client-side matcher and Router behavior (tst_routing), and
# the edge's application-shell fallback with its security headers (tst_spafallback).
# Native host kit; no browser needed.

cmake_minimum_required(VERSION 3.21)
project(synqt_url_routing LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Network HttpServer WebSockets Qml RemoteObjects Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")

enable_testing()

qt_add_executable(tst_routing tst_routing.cpp)

# The page components the route table points at. They live in the resource system, the
# same place a client's compiled-in views live, so the Router is exercised through real
# qrc URLs rather than a stub.
qt_add_resources(tst_routing "routing_fixtures"
    PREFIX "/fixtures"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    FILES
        fixtures/Home.qml
        fixtures/Summary.qml
        fixtures/Campaign.qml
        fixtures/Admin.qml
)

target_link_libraries(tst_routing PRIVATE SynQtClient Qt6::Qml Qt6::Test)
add_test(NAME routing COMMAND tst_routing)

qt_add_executable(tst_spafallback tst_spafallback.cpp)
target_link_libraries(tst_spafallback PRIVATE SynQtService Qt6::Test Qt6::Network)
add_test(NAME spafallback COMMAND tst_spafallback)
