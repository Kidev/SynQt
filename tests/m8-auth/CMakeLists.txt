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
# The Identity and SessionStore Sources this suite hosts come from SynQtIdentity: they are
# framework contracts (src/identity/contracts/), owned by the auth entity that
# `identity.provider_entity` names, so the library carries them and the generated auth main
# needs no app contract. This suite hosts them exactly as that generated main does, which is
# what makes it the runtime proof behind the generator.
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

# The desktop half of the same feature: the loopback redirect, the claim code, and a native
# client signing in end to end. It is the one suite that links both runtimes, because the
# flow is a conversation between them.
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

# Staying signed in on the desktop: the device credential, its rotation, and the reuse
# detection. Qt6::Sql not because the suite talks to a database, but because moving a row's
# timestamps is the only way to reach the far side of a window measured in days.
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

# The client half: what goes into the OS secure store, that nothing goes anywhere else, and
# that a second launch comes back signed in. Links both runtimes, because the last of those
# is a conversation between them.
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
    # The Linux store is the Secret Service, and no CI runner or container has one logged in,
    # so without help this test skips everywhere except a developer's desktop -- where it would
    # instead write a credential into the keyring they keep their life in. The wrapper gives it
    # a private bus and a private keyring, and falls through to running the binary directly
    # where those tools are absent, which leaves the skip exactly as it is today.
    add_test(NAME device-store
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../lib/keyring-session.sh"
                     "$<TARGET_FILE:tst_devicestore>")
else()
    add_test(NAME device-store COMMAND tst_devicestore)
endif()

# The env file behind every `env:` reference: it is what turns the NAME of a secret in
# synqt.yaml into its value at startup, so its precedence rules are part of the security
# story rather than a convenience. No contract, no certificates, no network.
qt_add_executable(tst_envfile tst_envfile.cpp)
target_link_libraries(tst_envfile PRIVATE SynQtService Qt6::Core Qt6::Test)
add_test(NAME envfile COMMAND tst_envfile)
