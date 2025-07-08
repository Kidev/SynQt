# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The client update default (the QML `App` accessor). Native: the decision itself is
# platform-independent, and the browser half needs a real WebAssembly build.

cmake_minimum_required(VERSION 3.21)
project(m6_clientupdate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Qml Test)

enable_testing()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")

add_executable(tst_clientupdate tst_clientupdate.cpp)
target_link_libraries(tst_clientupdate PRIVATE SynQtClient Qt6::Test)

add_test(NAME m6-clientupdate COMMAND tst_clientupdate)

add_executable(tst_qmlsyntax tst_qmlsyntax.cpp)
target_link_libraries(tst_qmlsyntax PRIVATE SynQtClient Qt6::Qml Qt6::Test)

add_test(NAME m6-qmlsyntax COMMAND tst_qmlsyntax)
