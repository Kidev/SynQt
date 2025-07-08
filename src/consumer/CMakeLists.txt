# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt consumer surface, shared by both the client runtime and the service runtime (a
# leaf library both depend on, like SynQtTransport). It holds the per-contract consumer
# facade base, the connect-point resolver behind `<Contract>.on<Signal>` attached handlers,
# and the Promise returned by a returning-slot call. The generated `<Contract>Consumer`
# facades derive from ConsumerBase and register themselves through this library. Qt Core +
# Qml (QJSValue) + RemoteObjects (the pending call) here; nothing service- or client-specific.

# GLOBAL so the imported Qt targets this library PUBLIC-links (notably Qt6::Qml, used in the
# public promise/facade headers via QJSValue) are visible when a consuming executable is
# finalized, even if that executable's own find_package did not list the component. Without
# it, Qt's link-interface walk warns "the Qml target is mentioned as a dependency but not
# declared" for every such consumer. This library is pulled in via add_subdirectory, so the
# promotion is the leaf's responsibility, not each consumer's.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Qml RemoteObjects GLOBAL)

qt_add_library(SynQtConsumer STATIC
    connectpointresolver.cpp
    connectpointresolver.h
    consumerbase.cpp
    consumerbase.h
    consumerfactory.cpp
    consumerfactory.h
    promise.cpp
    promise.h
)

set_target_properties(SynQtConsumer PROPERTIES AUTOMOC ON)

target_include_directories(SynQtConsumer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SynQtConsumer PUBLIC
    Qt6::Core
    Qt6::Qml
    Qt6::RemoteObjects
)
