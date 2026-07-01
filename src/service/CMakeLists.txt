# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt service runtime core, linked into every native entity. It provides the mesh
# transport (mutual TLS by default, opt-in local socket), the entity runtime and
# connect-point host (topology resolution, deny by default), sessions and the Caller
# accessor.
#
# HTTP and OAuth live in ../edge and ../identity, because those Qt modules are GPLv3 only
# (docs/licensing.md).

# GLOBAL: the executable that links this library is finalized in another directory scope,
# where the Qt targets it links publicly must still resolve.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Qml RemoteObjects WebSockets GLOBAL)
find_package(OpenSSL REQUIRED)

# The shared transport adapter (guarded so several entities can pull it in once).
if(NOT TARGET SynQtTransport)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../transport" "${CMAKE_BINARY_DIR}/SynQtTransport")
endif()

# The shared consumer surface (facade base, connect-point resolver, Promise): a service that
# consumes another entity's connect point reaches it through the generated facade.
if(NOT TARGET SynQtConsumer)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../consumer" "${CMAKE_BINARY_DIR}/SynQtConsumer")
endif()

# The provider layer (family interfaces, bundled providers, Db/Cache/Docs/Http/Jobs helpers).
# EntityRuntime injects the type's helper into each owned Source; a service links this,
# the client never does.
if(NOT TARGET SynQtProviders)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../providers" "${CMAKE_BINARY_DIR}/SynQtProviders")
endif()

qt_add_library(SynQtService STATIC
    meshpeer.h
    localpeer.cpp
    localpeer.h
    meshserver.cpp
    meshserver.h
    meshclient.cpp
    meshclient.h
    topology.cpp
    topology.h
    tracer.cpp
    tracer.h
    clientaddress.cpp
    clientaddress.h
    cookies.h
    connectpointhost.cpp
    connectpointhost.h
    entityruntime.cpp
    entityruntime.h
    eventring.cpp
    eventring.h
    envfile.cpp
    envfile.h
    ingestclient.cpp
    ingestclient.h
    log.cpp
    log.h
    sessionmanager.cpp
    sessionmanager.h
    caller.cpp
    caller.h
    callspan.cpp
    callspan.h
    sourcefactory.cpp
    sourcefactory.h
    traceevent.cpp
    traceevent.h
)

set_target_properties(SynQtService PROPERTIES AUTOMOC ON)

target_include_directories(SynQtService PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SynQtService PUBLIC
    SynQtTransport
    SynQtConsumer
    SynQtProviders
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    OpenSSL::Crypto
)
