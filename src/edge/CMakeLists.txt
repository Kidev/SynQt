# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt web edge: the one entity a browser can reach. It serves the client bundle with
# the header policy, runs the WebSocket upgrade pipeline (origin, session, scope, rate and
# size checks before a socket exists), carries the login routes that drive the identity
# engine in ../identity, and owns the framework's own connect points (Pages and
# SessionState).
#
# Its own library because Qt HTTP Server is GPLv3-only: an entity that links this is GPLv3.
# Only a `type: web_edge` entity does. A service that merely consumes the edge's connect
# points links ../service and stays LGPLv3 (docs/licensing.md).

# GLOBAL: see the note in ../service/CMakeLists.txt.
find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects RemoteObjectsPrivate WebSockets HttpServer GLOBAL)

if(NOT TARGET SynQtIdentity)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../identity" "${CMAKE_BINARY_DIR}/SynQtIdentity")
endif()

# synqt_add_contract, for the Pages connect point below: it is an ordinary contract owner,
# generated the same way an app's connect points are.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

qt_add_library(SynQtEdge STATIC
    identitymapping.h
    identityprovider.cpp
    identityprovider.h
    pageseed.h
    pagestore.cpp
    pagestore.h
    pagesservice.cpp
    pagesservice.h
    pagesedgesource.cpp
    pagesedgesource.h
    sessionstatesource.cpp
    sessionstatesource.h
    webedgeconfig.h
    webedge.cpp
    webedge.h
    withdrawsource.cpp
    withdrawsource.h
)

set_target_properties(SynQtEdge PROPERTIES AUTOMOC ON)

# The development sign-in, compiled only with SYNQT_DEV_TOOLS (docs/security.md).
if(SYNQT_DEV_TOOLS)
    target_sources(SynQtEdge PRIVATE
        identitypicker.cpp
        identitypicker.h
        stubidentityserver.cpp
        stubidentityserver.h
    )
endif()

target_include_directories(SynQtEdge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The framework's own connect points, Pages and SessionState. The generated headers are
# PUBLIC so no linker of this library generates a second copy.
synqt_add_contract(SynQtEdge ROLE source
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/contracts/Pages.syn"
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/SessionState.syn")
target_include_directories(SynQtEdge PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/synqt_generated/SynQtEdge"
    "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(SynQtEdge PUBLIC
    SynQtIdentity
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
)

# withdrawsource.cpp reaches into QtRemoteObjects' private state, which is tied to the pinned
# Qt. PRIVATE, so no other file includes a private header by accident.
target_link_libraries(SynQtEdge PRIVATE Qt6::RemoteObjectsPrivate)
