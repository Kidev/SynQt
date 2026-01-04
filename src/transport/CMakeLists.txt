# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The shared QWebSocket QIODevice adapter used by both the client runtime (to reach the
# edge) and the web edge (to wrap accepted browser sockets); the socket options every
# SynQt link is opened with; RoutePattern, the route
# matcher both the client (navigation) and a service (fetchPage authorization) compile a
# request path against; and the proxy policy every server-side manager takes. A leaf
# library so both sides can depend on it without one depending on the other.

# GLOBAL: see the note in src/service/CMakeLists.txt. Qt6::WebSockets is public on this target,
# so it has to resolve in whatever scope the consuming executable is finalized in.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network WebSockets GLOBAL)

qt_add_library(SynQtTransport STATIC
    websockettransport.cpp
    websockettransport.h
    socketoptions.cpp
    socketoptions.h
    iothreadpool.cpp
    iothreadpool.h
    socketchannel.cpp
    socketchannel.h
    routepattern.cpp
    routepattern.h
    proxypolicy.cpp
    proxypolicy.h
)

set_target_properties(SynQtTransport PROPERTIES AUTOMOC ON)

target_include_directories(SynQtTransport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SynQtTransport PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
)
