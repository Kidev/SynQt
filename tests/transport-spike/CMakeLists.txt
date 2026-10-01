# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The transport spike. The edge (native, host Qt kit) and the client (WASM, wasm
# kit) are built by separate CMake configure runs because they use different
# toolchains; this top-level file dispatches to exactly one entity per configure,
# selected by -DSYNQT_SPIKE_ENTITY=edge|client. Both share shared/ (the hand-written
# contract and the WebSocket QIODevice adapter).

cmake_minimum_required(VERSION 3.21)
project(synqt_transport_spike LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

# Land artifacts directly in the binary dir root so the verify harness can find
# build/spike-edge/spike-edge and build/spike-client/spike-client.html without guessing.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(SYNQT_SPIKE_ENTITY "" CACHE STRING "Which spike entity to build: edge or client")

if(SYNQT_SPIKE_ENTITY STREQUAL "edge")
    add_subdirectory(edge)
elseif(SYNQT_SPIKE_ENTITY STREQUAL "client")
    add_subdirectory(client)
else()
    message(FATAL_ERROR
        "Configure with -DSYNQT_SPIKE_ENTITY=edge or -DSYNQT_SPIKE_ENTITY=client")
endif()
