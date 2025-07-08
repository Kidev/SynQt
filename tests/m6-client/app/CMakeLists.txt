# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The counter example as buildable apps: the client (built for the browser via the WASM
# kit, or as a native desktop app from the same QML) and the edge server that serves the
# WASM bundle and hosts the counter. Configure with the host kit for the desktop client +
# edge, or with the wasm kit's qt-cmake for the browser client.

cmake_minimum_required(VERSION 3.21)
project(synqt_m6_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(SYNQT_EDGE_URL "wss://127.0.0.1:8443/sync" CACHE STRING "Desktop client edge URL")

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Qml Quick QuickControls2)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/SynQtContracts.cmake")

qt_standard_project_setup(REQUIRES 6.11)

# ---- The counter client (browser WASM and native desktop, from one QML) ----
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../client/Main.qml"
    PROPERTIES QT_RESOURCE_ALIAS Main.qml)
qt_add_executable(counter-client "${CMAKE_CURRENT_SOURCE_DIR}/../client/main.cpp")
qt_add_qml_module(counter-client
    URI CounterClient
    VERSION 1.0
    QML_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../client/Main.qml"
)
# The client consumes the counter connect point, so it generates the typed Replica (and
# its factory registration), never the Source helper.
synqt_add_contract(counter-client ROLE replica
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/../shared/Counter.syn")
target_compile_definitions(counter-client PRIVATE SYNQT_EDGE_URL="${SYNQT_EDGE_URL}")
target_link_libraries(counter-client PRIVATE
    SynQtClient Qt6::Core Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::RemoteObjects)
option(SYNQT_WASM_NO_EVAL "Build the WASM client without eval (CSP-strict)" ON)
if(EMSCRIPTEN)
    # The client reads window.location through the Embind JS bridge (no eval), and the
    # emscripten runtime is built without eval/new Function (DYNAMIC_EXECUTION=0), so the
    # edge's strict Content-Security-Policy (script-src without 'unsafe-eval') holds.
    target_link_options(counter-client PRIVATE "-lembind")
    if(SYNQT_WASM_NO_EVAL)
        target_link_options(counter-client PRIVATE "-sDYNAMIC_EXECUTION=0")
    endif()
endif()

# ---- The edge server (host only; not built for WASM) ----
if(NOT EMSCRIPTEN)
    find_package(Qt6 6.11 REQUIRED COMPONENTS Network RemoteObjects WebSockets HttpServer)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../src/service" "${CMAKE_BINARY_DIR}/SynQtService")

    qt_add_executable(counter-edge "${CMAKE_CURRENT_SOURCE_DIR}/../edge/main.cpp")
    synqt_add_contract(counter-edge ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/../shared/Counter.syn")
    target_link_libraries(counter-edge PRIVATE
        SynQtService Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml
        Qt6::RemoteObjects Qt6::WebSockets Qt6::HttpServer)
endif()
