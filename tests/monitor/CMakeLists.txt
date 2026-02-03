# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Monitoring acceptance: the event pipeline every entity carries. The record, the bounded
# ring that drops rather than blocks, the tracer and its per-category levels, and the
# writer thread that batches off the caller's thread.
#
# Qt Core and Qt Test only, on purpose: the pipeline lives in SynQtService, which links
# nothing GPLv3, and a suite that quietly pulled in Qt Sql or Qt HTTP Server would stop
# proving that.

cmake_minimum_required(VERSION 3.21)
project(synqt_monitor LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Test)
qt_standard_project_setup()

# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtService)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
endif()

enable_testing()

qt_add_executable(tst_pipeline tst_pipeline.cpp)

target_link_libraries(tst_pipeline PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Test
)

add_test(NAME monitor-pipeline COMMAND tst_pipeline)
