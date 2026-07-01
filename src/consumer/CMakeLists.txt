# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt consumer surface, shared by both the client runtime and the service runtime (a
# leaf library both depend on, like SynQtTransport). It holds the per-contract consumer
# facade base, the connect-point resolver behind `<Contract>.on<Signal>` attached handlers,
# and the Promise returned by a returning-slot call. The generated `<Contract>Consumer`
# facades derive from ConsumerBase and register themselves through this library. Qt Core +
# Qml (QJSValue) + RemoteObjects (the pending call) here; nothing service- or client-specific.

# GLOBAL: see the note in src/service/CMakeLists.txt.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Qml RemoteObjects GLOBAL)

qt_add_library(SynQtConsumer STATIC
    actingfor.cpp
    actingfor.h
    connectpointresolver.cpp
    connectpointresolver.h
    consumerbase.cpp
    consumerbase.h
    consumerfactory.cpp
    consumerfactory.h
    deletesoon.cpp
    deletesoon.h
    moduleimports.cpp
    moduleimports.h
    promise.cpp
    promise.h
    readonlymodel.cpp
    readonlymodel.h
    tracecontext.cpp
    tracecontext.h
    tracescope.cpp
    tracescope.h
)

set_target_properties(SynQtConsumer PROPERTIES AUTOMOC ON)

target_include_directories(SynQtConsumer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SynQtConsumer PUBLIC
    Qt6::Core
    Qt6::Qml
    Qt6::RemoteObjects
)
