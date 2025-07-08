# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M0 transport spike. The edge (native, host Qt kit) and the client (WASM, wasm
# kit) are built by separate CMake configure runs because they use different
# toolchains; this top-level file dispatches to exactly one entity per configure,
# selected by -DSYNQT_M0_ENTITY=edge|client. Both share shared/ (the hand-written
# contract and the WebSocket QIODevice adapter).

cmake_minimum_required(VERSION 3.21)
project(synqt_m0_transport LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Land artifacts directly in the binary dir root so the verify harness can find
# build/m0-edge/m0-edge and build/m0-client/m0-client.html without guessing.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(SYNQT_M0_ENTITY "" CACHE STRING "Which M0 entity to build: edge or client")

if(SYNQT_M0_ENTITY STREQUAL "edge")
    add_subdirectory(edge)
elseif(SYNQT_M0_ENTITY STREQUAL "client")
    add_subdirectory(client)
else()
    message(FATAL_ERROR
        "Configure with -DSYNQT_M0_ENTITY=edge or -DSYNQT_M0_ENTITY=client")
endif()
