# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# FIX-1 acceptance: the auction tutorial's owner-side authorization, proven end to end on
# the real three-entity gavel system (examples/gavel). The web edge owns the live auction
# (one Source per caller, so Caller is the bidding user) and authorizes every bid; the database owns
# the durable ledger and authorizes the calling entity (Caller.entity === "edge"). The
# client is a browser presenting a session, no secret and no mesh certificate. Native host
# kit; throwaway CA + entity certs and a localhost edge cert are generated at configure
# time. The connect-point Sources under test are the example's own files.

cmake_minimum_required(VERSION 3.21)
project(synqt_fix1_auction LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.11 REQUIRED COMPONENTS
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

# What crosses gavel's connect points is written on the points, in its synqt.yaml, so the
# `.syn` the compiler reads is generated. `synqt build` does this as its first step; this
# fixture drives CMake directly, so it asks for that one step by itself.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../write-contracts.py" "${GAVEL}"
    RESULT_VARIABLE _syn_rc ERROR_VARIABLE _syn_err)
if(NOT _syn_rc EQUAL 0)
    message(FATAL_ERROR "FIX-1: could not write gavel's contracts:\n${_syn_err}")
endif()

set(FIX1_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${FIX1_CERTS}"
    RESULT_VARIABLE _cert_rc ERROR_VARIABLE _cert_err)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "FIX-1 certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_fix1 tst_fix1.cpp)

# The web edge hosts AuctionSource + HallSource from its QML; the books entity hosts
# LedgerSource. Every consumer uses dynamic replicas, so no replica headers are generated.
synqt_add_contract(tst_fix1 ROLE source
    SYN "${GAVEL}/generated/web/edge/Auction.syn")
# The ledger is the mesh half: the edge consumes it, so it needs the consumer facade
# (ROLE both) and its slots carry the session the edge is acting for (FORWARDS_SESSION),
# exactly as `synqt build` compiles it for the two entities separately.
synqt_add_contract(tst_fix1 ROLE both FORWARDS_SESSION
    SYN "${GAVEL}/generated/db/relational/books/Ledger.syn")
synqt_add_contract(tst_fix1 ROLE source SYN "${GAVEL}/generated/web/edge/Hall.syn")

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

add_test(NAME fix1 COMMAND tst_fix1)
