# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt inbound HTTP surface: what an entity gets when its `network.inbound` says it
# serves one. `ApiServer` runs the checks a public caller can influence (rate, API key,
# origin, body size) before anything reaches QML, and `Api` is the helper the entity's own
# singleton declares its routes on.
#
# Its own library because Qt HTTP Server is GPLv3-only: an entity that serves no inbound
# surface must not link it, and with this split it does not. It deliberately does NOT link
# ../identity: a gateway authenticates machine callers with a key, not with OAuth, so it
# has no reason to carry Qt Network Authorization (docs/licensing.md).

# GLOBAL: see the note in ../service/CMakeLists.txt.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network Qml HttpServer GLOBAL)

if(NOT TARGET SynQtService)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../service" "${CMAKE_BINARY_DIR}/SynQtService")
endif()

qt_add_library(SynQtGateway STATIC
    apiconfig.h
    apirequest.cpp
    apirequest.h
    api.cpp
    api.h
    apiserver.cpp
    apiserver.h
)

set_target_properties(SynQtGateway PROPERTIES AUTOMOC ON)

target_include_directories(SynQtGateway PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SynQtGateway PUBLIC
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::HttpServer
)
