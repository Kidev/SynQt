# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Remote pages acceptance: the edge-side page store and its authorization (tst_pagestore),
# and the client-side palette validation and cache (tst_remotepage). Native host kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_remote_pages LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")

enable_testing()

# Pages is the framework's own contract, generated once as part of SynQtService itself
# (see src/service/CMakeLists.txt) and exposed publicly from there; this test reaches
# "rep_pages_source.h" and PagesService through that link rather than generating (and
# then linking) a second, colliding copy of PagesSourceHelper/PagesCaller of its own.
qt_add_executable(tst_pagestore tst_pagestore.cpp)
target_link_libraries(tst_pagestore PRIVATE SynQtService Qt6::Test)
target_compile_definitions(tst_pagestore PRIVATE
    REMOTE_PAGES_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME pagestore COMMAND tst_pagestore)

qt_add_executable(tst_remotepage tst_remotepage.cpp)
target_link_libraries(tst_remotepage PRIVATE SynQtClient Qt6::Test Qt6::Qml)
add_test(NAME remotepage COMMAND tst_remotepage)
