# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt web edge: the one entity a browser can reach. It serves the client bundle with
# the header policy, runs the WebSocket upgrade pipeline (origin, session, scope, rate and
# size checks before a socket exists), carries the login routes that drive the identity
# engine in ../identity, and owns the framework's own Pages connect point.
#
# Its own library because Qt HTTP Server is GPLv3-only: an entity that links this is GPLv3.
# Only a `type: web_edge` entity does. A service that merely consumes the edge's connect
# points links ../service and stays LGPLv3 (docs/licensing.md).

# GLOBAL: see the note in ../service/CMakeLists.txt.
find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Network Qml RemoteObjects WebSockets HttpServer GLOBAL)

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
    stubidentityserver.cpp
    stubidentityserver.h
    pageseed.h
    pagestore.cpp
    pagestore.h
    pagesservice.cpp
    pagesservice.h
    pagesedgesource.cpp
    pagesedgesource.h
    webedgeconfig.h
    webedge.cpp
    webedge.h
)

set_target_properties(SynQtEdge PROPERTIES AUTOMOC ON)

target_include_directories(SynQtEdge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The framework's own Pages connect point (edge-delivered pages): PagesService's
# fetchPageFor returns the generated PageResponse record, so the library needs the
# owner-side generated code for its own compile, exactly as an app entity would for its
# own connect points. The generated header is exposed PUBLIC (unlike a normal
# per-executable synqt_add_contract call) so a consumer needing "rep_pages_source.h"
# (e.g. a test constructing a PageResponse) finds it without generating and linking its
# own duplicate copy of PagesSourceHelper/PagesCaller alongside this static library's.
synqt_add_contract(SynQtEdge ROLE source
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/contracts/Pages.syn")
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
