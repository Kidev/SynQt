# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt client runtime library. It links into the WebAssembly client and the native
# desktop client (the same QML), and holds no secret and no mesh certificate. It reaches
# services only through the edge. Qt Core + Network + WebSockets + Qml + RemoteObjects
# here; never HttpServer, NetworkAuth, or Sql (the client never listens, holds secrets,
# or touches storage).

# GLOBAL so the imported Qt targets this library PUBLIC-links (notably Qt6::Qml, inherited
# through the public consumer surface) stay visible when a consuming executable is finalized,
# even if that executable's own find_package did not list the component. See the note in
# src/consumer/CMakeLists.txt: the promotion belongs to the add_subdirectory'd leaf, not each
# consumer, and prevents the "Qml target mentioned as a dependency but not declared" warning.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network WebSockets Qml RemoteObjects GLOBAL)

# The shared transport adapter (guarded so several entities can pull it in once).
if(NOT TARGET SynQtTransport)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../transport" "${CMAKE_BINARY_DIR}/SynQtTransport")
endif()

# The shared consumer surface (facade base, connect-point resolver, Promise): the generated
# `<Contract>Consumer` facades the client acquires derive from it.
if(NOT TARGET SynQtConsumer)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../consumer" "${CMAKE_BINARY_DIR}/SynQtConsumer")
endif()

qt_add_library(SynQtClient STATIC
    synclientconfig.h
    clientlogging.cpp
    clientlogging.h
    clientupdate.cpp
    clientupdate.h
    replicaregistry.cpp
    replicaregistry.h
    session.cpp
    session.h
    router.cpp
    router.h
    serveraccessor.cpp
    serveraccessor.h
    synclient.cpp
    synclient.h
)

set_target_properties(SynQtClient PROPERTIES AUTOMOC ON)

target_include_directories(SynQtClient PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SynQtClient PUBLIC
    SynQtTransport
    SynQtConsumer
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Qml
    Qt6::RemoteObjects
)
