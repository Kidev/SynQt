# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M8 acceptance: the edge login/callback flow (Authorization Code + PKCE + framework
# state + edge-held secret), identity normalization, the scope-mapping hook, the httpOnly
# session cookie, and the dev-stub gate. Driven end to end against the in-process dev stub
# provider. Native host kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_m8_auth LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Gui Network NetworkAuth Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtService)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(M8_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${M8_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "M8 certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_m8 tst_m8.cpp)
# The Identity and Session Sources this suite hosts come from SynQtService: they are
# framework contracts (src/service/contracts/), owned by the auth entity that
# `identity.provider_entity` names, so the library carries them and the generated auth main
# needs no app contract. This suite hosts them exactly as that generated main does, which is
# what makes it the runtime proof behind the generator.
target_compile_definitions(tst_m8 PRIVATE
    M8_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
    M8_CERT_DIR="${M8_CERTS}")
target_link_libraries(tst_m8 PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::NetworkAuth
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)

add_test(NAME m8 COMMAND tst_m8)

# The env file behind every `env:` reference: it is what turns the NAME of a secret in
# synqt.yaml into its value at startup, so its precedence rules are part of the security
# story rather than a convenience. No contract, no certificates, no network.
qt_add_executable(tst_envfile tst_envfile.cpp)
target_link_libraries(tst_envfile PRIVATE SynQtService Qt6::Core Qt6::Test)
add_test(NAME envfile COMMAND tst_envfile)
