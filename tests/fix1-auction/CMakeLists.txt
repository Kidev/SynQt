# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The auction tutorial's owner-side authorization on the real gavel system.
# The edge authorizes each bid by user; the books entity authorizes the calling entity. The
# Sources under test are the example's own files. Native host kit; throwaway certificates
# are generated at configure time.

cmake_minimum_required(VERSION 3.21)
project(synqt_fix1_auction LANGUAGES CXX)

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

set(GAVEL "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/gavel")

# Generate the `.syn` from gavel's synqt.yaml, the first step of `synqt build`.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../write-contracts.py" "${GAVEL}"
    RESULT_VARIABLE _syn_rc ERROR_VARIABLE _syn_err)
if(NOT _syn_rc EQUAL 0)
    message(FATAL_ERROR "fix1-auction: could not write gavel's contracts:\n${_syn_err}")
endif()

set(FIX1_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${FIX1_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "fix1-auction: certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_fix1 tst_fix1.cpp)

# The edge hosts EdgeSource, the books entity BooksSource; consumers use dynamic replicas.
synqt_add_contract(tst_fix1 ROLE source
    SYN "${GAVEL}/generated/web/edge/Edge.syn")
# Books: consumed by the edge (ROLE both), with the session on every call (FORWARDS_SESSION).
synqt_add_contract(tst_fix1 ROLE both FORWARDS_SESSION
    SYN "${GAVEL}/generated/db/relational/books/Books.syn")

target_compile_definitions(tst_fix1 PRIVATE
    FIX1_CERT_DIR="${FIX1_CERTS}"
    FIX1_GAVEL_DIR="${GAVEL}"
    FIX1_BUNDLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/bundle"
)

target_link_libraries(tst_fix1 PRIVATE
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

# The books entity runs as its own process: `Books` is a Source there and an accessor in the edge.
add_subdirectory(books)
add_dependencies(tst_fix1 fix1_books)
target_compile_definitions(tst_fix1 PRIVATE
    FIX1_BOOKS_BIN="$<TARGET_FILE:fix1_books>")

add_test(NAME fix1 COMMAND tst_fix1)
