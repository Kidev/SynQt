# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The client frame-time scene: a pure 2D Qt Quick app (no QtRO, so it builds against the stock
# WebAssembly kit without the from-source RemoteObjects step) that renders a growing field of
# moving, interpolated blobs. Built for both WASM kits by run-bench.sh and driven by frame-time.mjs,
# it isolates client render cost as the number of entities in view grows, single- vs multi-threaded.

cmake_minimum_required(VERSION 3.21)
project(synqt_client_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.11)

qt_add_executable(clientbench main.cpp framereporter.h)

# The blob ceiling and ramp are baked here so the WASM page carries them (no runtime environment in
# the browser); run-bench.sh overrides them per sweep with -DSYNQT_BENCH_BLOBS / -DSYNQT_BENCH_RAMP.
set(SYNQT_BENCH_BLOBS 800 CACHE STRING "Maximum blobs the scene ramps to")
set(SYNQT_BENCH_RAMP 12 CACHE STRING "Seconds over which the blob count ramps to the maximum")
target_compile_definitions(clientbench PRIVATE
    SYNQT_BENCH_BLOBS_DEFAULT=${SYNQT_BENCH_BLOBS}
    SYNQT_BENCH_RAMP_DEFAULT=${SYNQT_BENCH_RAMP})

qt_add_qml_module(clientbench
    URI synqtclientbench
    VERSION 1.0
    QML_FILES Main.qml
)

target_link_libraries(clientbench PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
)
