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

qt_add_library(SynQtMonitor STATIC
    eventstore.cpp
    eventstore.h
    operatorstore.cpp
    operatorstore.h
)

set_target_properties(SynQtMonitor PROPERTIES AUTOMOC ON)

target_include_directories(SynQtMonitor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SynQtMonitor PUBLIC
    SynQtEdge
    Qt6::Core
    Qt6::Sql
)
