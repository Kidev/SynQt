# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The 3D plaza tutorial's movement authority, its collisions and its
# sign-in gate. The web edge loads the example's own Edge.qml (examples/plaza) and moves
# every walker itself from what the keys ask for; `scope: user` on the connect point means a
# visitor who has not signed in never acquires the plaza at all. Native host kit; a
# localhost edge cert is generated at configure time.

cmake_minimum_required(VERSION 3.21)
project(synqt_plaza LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core Gui Network Qml RemoteObjects WebSockets HttpServer Test)
qt_standard_project_setup()

# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtEdge)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/edge" "${CMAKE_BINARY_DIR}/SynQtEdge")
endif()
if(NOT TARGET SynQtClient)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(PLAZA "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/plaza")

# What crosses the plaza's connect point is written on the point, in its synqt.yaml, so the
# `.syn` the compiler reads is generated. `synqt build` does this as its first step; this
# fixture drives CMake directly, so it asks for that one step by itself.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../write-contracts.py" "${PLAZA}"
    RESULT_VARIABLE _syn_rc ERROR_VARIABLE _syn_err)
if(NOT _syn_rc EQUAL 0)
    message(FATAL_ERROR "plaza: could not write the plaza's contracts:\n${_syn_err}")
endif()

set(PLAZA_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${PLAZA_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "plaza: certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_plaza tst_plaza.cpp)

# The web edge hosts EdgeSource from the example's QML; the clients use dynamic replicas.
synqt_add_contract(tst_plaza ROLE source
    SYN "${PLAZA}/generated/web/edge/Edge.syn")

target_compile_definitions(tst_plaza PRIVATE
    PLAZA_CERT_DIR="${PLAZA_CERTS}"
    PLAZA_DIR="${PLAZA}"
    PLAZA_BUNDLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/bundle"
)

target_link_libraries(tst_plaza PRIVATE
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

add_test(NAME plaza COMMAND tst_plaza)
