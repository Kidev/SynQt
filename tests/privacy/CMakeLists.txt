# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The privacy components: the Privacy accessor and the three QML types it backs
# (LegalFooter, CookieConsent, DataErasureRequest).

cmake_minimum_required(VERSION 3.21)
project(synqt_privacy LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Qml RemoteObjects WebSockets Test)
qt_standard_project_setup()

if(NOT TARGET SynQtClient)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
endif()

enable_testing()

qt_add_executable(tst_privacy tst_privacy.cpp)
target_link_libraries(tst_privacy PRIVATE SynQtClient Qt6::Test)
add_test(NAME privacy COMMAND tst_privacy)

# The components themselves need a scene graph, so they need Quick and a platform plugin.
# Split out for the reason tests/graphics splits the same way: the accessor above has to run
# on a kit that has no Qt Quick, and it does not need one.
find_package(Qt6 6.12 QUIET COMPONENTS Quick QuickControls2)
if(TARGET Qt6::Quick AND TARGET Qt6::QuickControls2)
    qt_add_executable(tst_privacycomponents tst_privacycomponents.cpp)
    target_link_libraries(tst_privacycomponents PRIVATE
        SynQtClient Qt6::Quick Qt6::QuickControls2 Qt6::Test)
    add_test(NAME privacycomponents COMMAND tst_privacycomponents)
    set_tests_properties(privacycomponents PROPERTIES
        ENVIRONMENT "QT_QUICK_BACKEND=software;QT_QPA_PLATFORM=offscreen")
endif()
