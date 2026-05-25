# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The framework's own build: every runtime library once, and every test suite that runs on
# the host kit.
#
# `synqt build` never reads this file. Needs a Qt 6.12 host kit with HttpServer and
# NetworkAuth, OpenSSL, and jwt-cpp 0.7.1 or newer.

cmake_minimum_required(VERSION 3.21)
project(SynQt VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core)
qt_standard_project_setup()

option(SYNQT_BUILD_TESTS "Build the host-kit test suites under tests/" ON)

include(cmake/SynQtCoverage.cmake)

# Leaves first. Each directory guards its dependencies with `if(NOT TARGET ...)`.
add_subdirectory(src/transport)
add_subdirectory(src/contract)
add_subdirectory(src/consumer)
add_subdirectory(src/providers)
add_subdirectory(src/client)
add_subdirectory(src/service)
add_subdirectory(src/identity)
add_subdirectory(src/edge)
add_subdirectory(src/gateway)
# After src/service: the harness links it, and this is the scope that creates the target.
add_subdirectory(src/testing)

# Only here: a suite configured standalone builds uninstrumented.
synqt_enable_coverage(
    SynQtTransport
    SynQtContract
    SynQtConsumer
    SynQtProviders
    SynQtClient
    SynQtService
    SynQtIdentity
    SynQtEdge
    SynQtGateway
    SynQtTesting
)

if(SYNQT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
