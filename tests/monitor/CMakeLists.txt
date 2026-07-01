# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Monitoring acceptance: the record, the bounded ring, the tracer and its levels, and the
# writer thread. `tst_pipeline` links only Qt Core and Qt Test, keeping the pipeline free
# of GPLv3 modules; `tst_instrumentation` drives a real web edge and session store.

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

# ROLE source: the test drives the generated Source helper, where the call span lives.
synqt_add_contract(tst_instrumentation ROLE source SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Hall.syn")
# And one a service would own: its slots take the session the calling entity is acting for,
# and with it the span that call continues.
synqt_add_contract(tst_instrumentation ROLE source FORWARDS_SESSION
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/shared/Ledger.syn")

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

# The reporting half, with no monitor present: a stand-in for the Replica.
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

# Who may read the console: the monitor's own identity system, kept separate from the
# application's.
qt_add_executable(tst_operator tst_operator.cpp)

target_link_libraries(tst_operator PRIVATE
    SynQtMonitor
    Qt6::Core
    Qt6::Test
)

add_test(NAME monitor-operator COMMAND tst_operator)

# The cold tier: the encoding, and not blocking when no collector answers.
qt_add_executable(tst_export tst_export.cpp)

target_link_libraries(tst_export PRIVATE
    SynQtMonitor
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)

add_test(NAME monitor-export COMMAND tst_export)
