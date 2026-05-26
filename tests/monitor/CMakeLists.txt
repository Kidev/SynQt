# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Monitoring acceptance: the event pipeline every entity carries. The record, the bounded
# ring that drops rather than blocks, the tracer and its per-category levels, and the
# writer thread that batches off the caller's thread.
#
# Two executables, and the split is required. `tst_pipeline` links Qt Core and Qt Test and
# nothing else, which is what proves the pipeline stayed inside SynQtService and never grew
# a GPLv3 dependency. `tst_instrumentation` drives a real web edge and a real session store
# to prove the choke points actually record what they promise, and links what that needs.

cmake_minimum_required(VERSION 3.21)
project(synqt_monitor LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects WebSockets HttpServer Sql Test)
qt_standard_project_setup()

# Guarded: configured on its own, this suite is what pulls the runtime library in;
# configured from the tree root, the root has already added it (see tests/CMakeLists.txt).
if(NOT TARGET SynQtMonitor)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/monitor" "${CMAKE_BINARY_DIR}/SynQtMonitor")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

set(MONITOR_CERTS "${CMAKE_CURRENT_BINARY_DIR}/certs")
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-cert.sh" "${MONITOR_CERTS}"
    RESULT_VARIABLE _cert_rc
    ERROR_VARIABLE _cert_err
)
if(NOT _cert_rc EQUAL 0)
    message(FATAL_ERROR "monitor suite certificate generation failed:\n${_cert_err}")
endif()

enable_testing()

qt_add_executable(tst_pipeline tst_pipeline.cpp)

target_link_libraries(tst_pipeline PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Test
)

add_test(NAME monitor-pipeline COMMAND tst_pipeline)

qt_add_executable(tst_instrumentation tst_instrumentation.cpp)

# ROLE source: the test drives the generated Source helper directly, which is where the
# call span the contract compiler emits actually lives. No transport is involved, because
# what is under test is the generated body and not the link it would be reached over.
synqt_add_contract(tst_instrumentation ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Hall.syn")

target_compile_definitions(tst_instrumentation PRIVATE
    MONITOR_CERT_DIR="${MONITOR_CERTS}"
    MONITOR_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tst_instrumentation PRIVATE
    SynQtEdge
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    Qt6::Test
)

add_test(NAME monitor-instrumentation COMMAND tst_instrumentation)

# The reporting half, on its own: no transport, no monitor entity, a stand-in for the
# Replica. What it is about is what happens when the monitor is NOT there, so standing a
# real one up would be testing the wrong thing.
qt_add_executable(tst_ingest tst_ingest.cpp)

target_link_libraries(tst_ingest PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Test
)

add_test(NAME monitor-ingest COMMAND tst_ingest)

# The monitor's own history, on its own: no entities, no transport, a temporary file.
qt_add_executable(tst_store tst_store.cpp)

target_link_libraries(tst_store PRIVATE
    SynQtMonitor
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)

add_test(NAME monitor-store COMMAND tst_store)

# Who may read the console: the monitor's own identity system, which is deliberately not
# the application's.
qt_add_executable(tst_operator tst_operator.cpp)

target_link_libraries(tst_operator PRIVATE
    SynQtMonitor
    Qt6::Core
    Qt6::Test
)

add_test(NAME monitor-operator COMMAND tst_operator)

# The cold tier: the encoding a third-party collector reads, and what happens when there is
# nothing at the other end. No collector is stood up, because what is under test is the
# serialization and the refusal to block, and a running collector would hide both.
qt_add_executable(tst_export tst_export.cpp)

target_link_libraries(tst_export PRIVATE
    SynQtMonitor
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)

add_test(NAME monitor-export COMMAND tst_export)
