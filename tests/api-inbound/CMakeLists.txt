# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The inbound HTTP surface an entity gets from `network.inbound`: routes declared in QML on
# the `Api` helper, and the checks ApiServer runs before any of them is reached (API key,
# origin, body size, rate limit). Native host kit; no certificates, because what is under
# test is the request pipeline and TLS is QSslServer's job, proven in m5-webedge.

cmake_minimum_required(VERSION 3.21)
project(synqt_api_inbound LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Qml HttpServer Test)
qt_standard_project_setup()

if(NOT TARGET SynQtGateway)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/gateway"
        "${CMAKE_BINARY_DIR}/SynQtGateway")
endif()

enable_testing()

qt_add_executable(tst_apiinbound tst_apiinbound.cpp)

target_compile_definitions(tst_apiinbound PRIVATE
    APIINBOUND_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(tst_apiinbound PRIVATE
    SynQtGateway
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::Test
)

add_test(NAME apiinbound COMMAND tst_apiinbound)
