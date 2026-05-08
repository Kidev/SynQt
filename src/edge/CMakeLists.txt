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
)

set_target_properties(SynQtEdge PROPERTIES AUTOMOC ON)

# The development sign-in, and only in a development build.
#
# Not a runtime branch. A capability that exists in the binary can be reached through a bug
# in whatever checks the flag, through an argument someone passes, or simply read out of the
# strings by anyone holding the artifact; a file CMake never names is not compiled, not
# linked, and not there. `synqt dev` configures with SYNQT_DEV_TOOLS=ON and `synqt build`
# never does, the header refuses to be included without it, and the generated main does not
# emit the call for a release profile. tests/dev-exclusion reads the symbol tables of both
# builds and proves the difference. See cmake/SynQtBuildFlags.cmake and docs/security.md.
if(SYNQT_DEV_TOOLS)
    target_sources(SynQtEdge PRIVATE
        identitypicker.cpp
        identitypicker.h
        stubidentityserver.cpp
        stubidentityserver.h
    )
endif()

target_include_directories(SynQtEdge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The framework's own connect points, generated the same way an app's are: Pages
# (edge-delivered pages) and SessionState (who this connection's visitor is). PagesService's
# fetchPageFor returns the generated PageResponse record, so the library needs the
# owner-side generated code for its own compile, exactly as an app entity would for its
# own connect points. The generated header is exposed PUBLIC (unlike a normal
# per-executable synqt_add_contract call) so a consumer needing "rep_pages_source.h"
# (e.g. a test constructing a PageResponse) finds it without generating and linking its
# own duplicate copy of PagesSourceHelper/PagesCaller alongside this static library's.
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
