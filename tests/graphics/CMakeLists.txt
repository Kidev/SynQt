# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The graphics fallback: the probe, the watcher that recognises Qt declining to draw, the
# notice, and the per-type fixtures that keep the watcher's recognition honest.

cmake_minimum_required(VERSION 3.21)
project(synqt_graphics LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network Qml RemoteObjects WebSockets Test)
qt_standard_project_setup()

if(NOT TARGET SynQtClient)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
endif()

enable_testing()

qt_add_executable(tst_graphics tst_graphics.cpp)
target_link_libraries(tst_graphics PRIVATE SynQtClient Qt6::Test)
add_test(NAME graphics COMMAND tst_graphics)

# The type fixtures need a real scene graph, so they need Quick, and Qt Quick 3D when the
# kit has it. Absent, the Quick3D case skips and says so rather than silently not running.
find_package(Qt6 6.11 QUIET COMPONENTS Quick Quick3D)
if(TARGET Qt6::Quick)
    qt_add_executable(tst_softwarebackend tst_softwarebackend.cpp)
    target_link_libraries(tst_softwarebackend PRIVATE SynQtClient Qt6::Quick Qt6::Test)
    if(TARGET Qt6::Quick3D)
        target_compile_definitions(tst_softwarebackend PRIVATE SYNQT_HAVE_QUICK3D)
    endif()
    add_test(NAME softwarebackend COMMAND tst_softwarebackend)
    # The whole point: these run on the raster adaptation.
    set_tests_properties(softwarebackend PROPERTIES
        ENVIRONMENT "QT_QUICK_BACKEND=software;QT_QPA_PLATFORM=offscreen")
endif()
