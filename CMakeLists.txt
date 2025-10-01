# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The framework's own build: every runtime library once, and every test suite that runs on
# the host kit.
#
# A SynQt *application* does not use this file. Each entity is its own binary configured
# from its own generated CMakeLists (see docs/build-system-and-cli.md), and `synqt build`
# never reads this one. This tree exists for two reasons. The first is that `cmake -S . -B
# build` is what anyone opening the repository tries first, and it had nothing to find. The
# second is cost: each tests/<suite>/CMakeLists.txt is a standalone project that pulls the
# runtime libraries in through add_subdirectory, so building them one script at a time
# compiles SynQtService, SynQtClient, and the leaves under them once per suite. Here they
# are compiled once for all of them.
#
# Prerequisites are the union of what the suites need: a Qt 6.11 host kit including
# HttpServer and NetworkAuth, OpenSSL, and jwt-cpp 0.7.1 or newer. One suite on its own
# needs only its own share, which is why the per-suite run-*.sh scripts still exist and
# still work; see tests/README.md.

cmake_minimum_required(VERSION 3.21)
project(SynQt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core)
qt_standard_project_setup()

option(SYNQT_BUILD_TESTS "Build the host-kit test suites under tests/" ON)

include(cmake/SynQtCoverage.cmake)

# Leaves first, then the two runtimes. Each of these already guards its own dependencies
# with `if(NOT TARGET ...)`, so the order decides only which directory scope creates a
# target, never whether it is created twice. The same guard is what lets a suite be
# configured standalone: there, the suite is the one that adds what it needs.
add_subdirectory(src/transport)
add_subdirectory(src/consumer)
add_subdirectory(src/providers)
add_subdirectory(src/client)
add_subdirectory(src/service)
# After src/service: the harness links it, and this is the scope that creates the target.
add_subdirectory(src/testing)

# From here rather than from each src/*/CMakeLists.txt, because coverage is a measurement
# of the whole tree against the whole suite, and this is the only file that has all of it.
# A suite configured standalone therefore builds uninstrumented, which is what you want
# when you are debugging one milestone rather than measuring the set.
synqt_enable_coverage(
    SynQtTransport
    SynQtConsumer
    SynQtProviders
    SynQtClient
    SynQtService
    SynQtTesting
)

if(SYNQT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
