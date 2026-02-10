# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt monitor: the entity every other entity reports to, and the one that serves the
# operator console. It owns the history (SQLite), the operator credentials, and the
# console-facing connect points.
#
# Its own library for two reasons. The license: it links Qt Sql, and through SynQtEdge it
# links Qt HTTP Server, so an entity that links this is GPLv3 and only a `type: monitor`
# entity does. And the shape: nothing an ordinary service does needs a store of every
# entity's events, so putting it in SynQtService would put a database engine into every
# binary in a project to serve one of them.

# GLOBAL: see the note in ../service/CMakeLists.txt.
find_package(Qt6 6.11 REQUIRED COMPONENTS
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
    monitorservice.cpp
    monitorservice.h
    operatorstore.cpp
    operatorstore.h
)

set_target_properties(SynQtMonitor PROPERTIES AUTOMOC ON)

target_include_directories(SynQtMonitor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The monitor's own two connect points: `ingest`, which every service reports through, and
# `console`, which the operator's client reads. They are framework contracts, not app
# contracts: no `synqt.yaml` declares them and no project carries an `Ingest.syn`, so the
# monitor's generated main calls the Source registrations from this library rather than
# compiling its own copy. That is what makes adding a monitor the one-line configuration
# change it is meant to be, and what keeps the console bundle independent of the topology it
# watches. Exposed PUBLIC for the same reason SynQtIdentity's are.
synqt_add_contract(SynQtMonitor ROLE source
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/contracts/Ingest.syn"
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/Console.syn")
target_include_directories(SynQtMonitor PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/synqt_generated/SynQtMonitor"
    "${CMAKE_CURRENT_BINARY_DIR}")
# PUBLIC, unlike the PRIVATE link synqt_add_contract makes. The monitor's generated main
# includes both Source helpers itself, to call their registration functions, and a helper
# header includes <sourcemodel.h>; without this that include resolves in this library and
# not in the binary that links it.
target_link_libraries(SynQtMonitor PUBLIC SynQtContract)

target_link_libraries(SynQtMonitor PUBLIC
    SynQtEdge
    Qt6::Core
    Qt6::Sql
)
