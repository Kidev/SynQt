# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The consumer facade acceptance: `<Contract>.on<Signal>` attached handlers and the
# returning-slot `.then(...)` promise, plus facade forwarding of a property, model, void
# slot and signal. In-process over the real WebSocketTransport, consumed from real QML.
# Native host kit; no edge, no TLS.

cmake_minimum_required(VERSION 3.21)
project(synqt_consumer_facade LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml RemoteObjects WebSockets Test)
qt_standard_project_setup()

# The client runtime (ServerAccessor + the shared consumer surface); it pulls in
# SynQtConsumer and SynQtTransport.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/client" "${CMAKE_BINARY_DIR}/SynQtClient")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

enable_testing()

qt_add_executable(tst_consumer tst_consumer.cpp)

# ROLE both: one process hosts the Widget Source and consumes it (Source helper + Replica +
# the consumer facade).
synqt_add_contract(tst_consumer ROLE both SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Widget.syn")

target_compile_definitions(tst_consumer PRIVATE SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(tst_consumer PRIVATE
    SynQtClient
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::Test
)

add_test(NAME consumer-facade COMMAND tst_consumer)
