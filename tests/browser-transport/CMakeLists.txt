# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The client's WebSocketTransport adapter, proven by acquiring a host
# Source through it over a real local plaintext WebSocket. Native host kit.

cmake_minimum_required(VERSION 3.21)
project(synqt_browser_transport LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network WebSockets RemoteObjects Test)
qt_standard_project_setup()

# The framework client runtime library that owns WebSocketTransport.
# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtClient)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
endif()

enable_testing()

qt_add_executable(tst_browser_transport tst_browser_transport.cpp tcplistener.h)

# A single both-sided test target; the rep has no POD, so separate source and replica
# headers coexist in one target without a duplicate-symbol clash.
qt_add_repc_sources(tst_browser_transport "${CMAKE_CURRENT_SOURCE_DIR}/browser_transport.rep")
qt_add_repc_replicas(tst_browser_transport "${CMAKE_CURRENT_SOURCE_DIR}/browser_transport.rep")

target_include_directories(tst_browser_transport PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(tst_browser_transport PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::WebSockets
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME browser-transport COMMAND tst_browser_transport)

# The transport-only unit cases (framing, partial reads, large messages, close
# handling). No contract and no QtRO node: the adapter is the subject,
# and tst_browser_transport above already answers whether QtRO rides on it.
qt_add_executable(tst_wstransport tst_wstransport.cpp)

target_link_libraries(tst_wstransport PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)

add_test(NAME wstransport COMMAND tst_wstransport)

# The split transport: the device on this thread, its socket on another one.
qt_add_executable(tst_threadedsocket tst_threadedsocket.cpp tcplistener.h)

target_link_libraries(tst_threadedsocket PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)

add_test(NAME threadedsocket COMMAND tst_threadedsocket)

# The threads a threaded entity spreads its sockets across.
qt_add_executable(tst_iothreads tst_iothreads.cpp)

target_link_libraries(tst_iothreads PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Test
)

add_test(NAME iothreads COMMAND tst_iothreads)

# The other half of SynQtTransport: which way an entity's outbound calls leave the host.
qt_add_executable(tst_proxypolicy tst_proxypolicy.cpp)

target_link_libraries(tst_proxypolicy PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Network
    Qt6::Test
)

add_test(NAME proxypolicy COMMAND tst_proxypolicy)

# The transport spike's client controller, natively: what a reconnect releases.
set(SYNQT_SPIKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../transport-spike")
qt_add_executable(tst_spikecontroller
    tst_spikecontroller.cpp
    "${SYNQT_SPIKE_DIR}/client/spikecontroller.cpp"
    "${SYNQT_SPIKE_DIR}/client/spikecontroller.h"
    "${SYNQT_SPIKE_DIR}/shared/websocketiodevice.cpp"
    "${SYNQT_SPIKE_DIR}/shared/websocketiodevice.h"
)

qt_add_repc_replicas(tst_spikecontroller "${SYNQT_SPIKE_DIR}/shared/spike.rep")

target_include_directories(tst_spikecontroller PRIVATE
    "${SYNQT_SPIKE_DIR}/client" "${SYNQT_SPIKE_DIR}/shared" "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(tst_spikecontroller PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::RemoteObjects
    Qt6::Test
)

add_test(NAME spikecontroller COMMAND tst_spikecontroller)
