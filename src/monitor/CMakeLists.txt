# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt monitor: the entity every other entity reports to, and the one that serves the
# operator console. It owns the history (SQLite), the operator credentials, and the
# console-facing connect points.
#
# Its own library: it links Qt HTTP Server (GPLv3) and Qt Sql, which no other service needs.

# GLOBAL: see the note in ../service/CMakeLists.txt.
find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects WebSockets HttpServer Sql GLOBAL)

if(NOT TARGET SynQtEdge)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../edge" "${CMAKE_BINARY_DIR}/SynQtEdge")
endif()

# synqt_add_contract, for the two connect points below: the monitor owns them the way an
# app entity owns its own, and they are generated the same way.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

qt_add_library(SynQtMonitor STATIC
    eventstore.cpp
    eventstore.h
    ieventexporter.cpp
    ieventexporter.h
    jsonlexporter.cpp
    jsonlexporter.h
    monitorservice.cpp
    monitorservice.h
    operatorstore.cpp
    operatorstore.h
    otlpexporter.cpp
    otlpexporter.h
)

set_target_properties(SynQtMonitor PROPERTIES AUTOMOC ON)

target_include_directories(SynQtMonitor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The framework contracts `ingest` and `console`. The generated main registers their
# Sources from this library.
synqt_add_contract(SynQtMonitor ROLE source
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/contracts/Ingest.syn"
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/Console.syn")
target_include_directories(SynQtMonitor PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/synqt_generated/SynQtMonitor"
    "${CMAKE_CURRENT_BINARY_DIR}")
# PUBLIC: the generated main includes the Source helpers, which include <sourcemodel.h>.
target_link_libraries(SynQtMonitor PUBLIC SynQtContract)

target_link_libraries(SynQtMonitor PUBLIC
    SynQtEdge
    Qt6::Core
    # OTLP over HTTP/JSON needs only a network client.
    Qt6::Network
    Qt6::Sql
)
