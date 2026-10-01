# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Network RemoteObjects WebSockets)
qt_standard_project_setup()

qt_add_executable(spike-edge
    main.cpp
    spikesource.cpp
    spikesource.h
    ../shared/websocketiodevice.cpp
    ../shared/websocketiodevice.h
)

# repc lowers the hand-written contract into the QtRO Source header for the owner.
qt_add_repc_sources(spike-edge ../shared/spike.rep)

target_include_directories(spike-edge PRIVATE ../shared ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(spike-edge PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::RemoteObjects
    Qt6::WebSockets
)
