# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The inbound HTTP surface an entity gets from `network.inbound`: routes declared in QML on
# the `Api` helper, and the checks ApiServer runs before any of them is reached (API key,
# origin, body size, rate limit), and the surface's own TLS: loading what it was named,
# serving over it, and refusing to start on what it cannot use. Native host kit. Throwaway
# certificates are generated at configure time into the build tree (never committed).

cmake_minimum_required(VERSION 3.21)
project(synqt_api_inbound LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Qml HttpServer Test)
qt_standard_project_setup()

if(NOT TARGET SynQtGateway)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/gateway"
        "${CMAKE_BINARY_DIR}/SynQtGateway")
endif()

set(APIINBOUND_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${APIINBOUND_CERTS}"
    RESULT_VARIABLE _certs_rc
    ERROR_VARIABLE _certs_err
)
if(NOT _certs_rc EQUAL 0)
    message(FATAL_ERROR "api-inbound certificate generation failed:\n${_certs_err}")
endif()

enable_testing()

qt_add_executable(tst_apiinbound tst_apiinbound.cpp)

target_compile_definitions(tst_apiinbound PRIVATE
    APIINBOUND_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
    APIINBOUND_CERT_DIR="${APIINBOUND_CERTS}")

target_link_libraries(tst_apiinbound PRIVATE
    SynQtGateway
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::Test
)

add_test(NAME apiinbound COMMAND tst_apiinbound)
