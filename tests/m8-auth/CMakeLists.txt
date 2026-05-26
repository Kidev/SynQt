# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The edge login flow (Authorization Code + PKCE + state + edge-held secret),
# identity normalization, the scope-mapping hook, the session cookie and the dev-stub gate,
# against the in-process dev stub. Native host kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_m8_auth LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core Gui Network NetworkAuth Qml RemoteObjects Sql WebSockets HttpServer Test)
qt_standard_project_setup()

# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtEdge)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/edge" "${CMAKE_BINARY_DIR}/SynQtEdge")
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
# The Identity and SessionStore Sources come from SynQtIdentity, hosted as the generated auth main does.
target_compile_definitions(tst_m8 PRIVATE
    M8_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
    M8_CERT_DIR="${M8_CERTS}")
target_link_libraries(tst_m8 PRIVATE
    SynQtEdge
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

# The desktop sign-in: loopback redirect, claim code, native client. Links both runtimes.
if(NOT TARGET SynQtClient)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
endif()
qt_add_executable(tst_desktop tst_desktop.cpp)
target_compile_definitions(tst_desktop PRIVATE M8_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tst_desktop PRIVATE
    SynQtEdge
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)
add_test(NAME desktop-signin COMMAND tst_desktop)

# The device credential. Qt6::Sql moves a row's timestamps to reach a window measured in days.
qt_add_executable(tst_device tst_device.cpp)
target_compile_definitions(tst_device PRIVATE M8_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tst_device PRIVATE
    SynQtEdge
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::Sql
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)
add_test(NAME device-credential COMMAND tst_device)

# The client half of the device credential. Links both runtimes.
qt_add_executable(tst_devicestore tst_devicestore.cpp)
target_compile_definitions(tst_devicestore PRIVATE M8_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tst_devicestore PRIVATE
    SynQtEdge
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)
if(UNIX AND NOT APPLE)
    # The wrapper gives the Linux store a private bus and keyring; without those tools the
    # test runs directly and skips.
    add_test(NAME device-store
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../lib/keyring-session.sh"
                     "$<TARGET_FILE:tst_devicestore>")
else()
    add_test(NAME device-store COMMAND tst_devicestore)
endif()

# The env file behind every `env:` reference, and its precedence.
qt_add_executable(tst_envfile tst_envfile.cpp)
target_link_libraries(tst_envfile PRIVATE SynQtService Qt6::Core Qt6::Test)
add_test(NAME envfile COMMAND tst_envfile)
