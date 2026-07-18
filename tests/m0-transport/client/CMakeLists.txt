# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network RemoteObjects WebSockets Qml Quick)
qt_standard_project_setup(REQUIRES 6.11)

qt_add_executable(m0-client
    main.cpp
    m0controller.cpp
    m0controller.h
    ../shared/websocketiodevice.cpp
    ../shared/websocketiodevice.h
)

# repc lowers the hand-written contract into the QtRO Replica header for the consumer.
qt_add_repc_replicas(m0-client ../shared/spike.rep)

qt_add_qml_module(m0-client
    URI M0Client
    VERSION 1.0
    QML_FILES Main.qml
)

target_include_directories(m0-client PRIVATE ../shared ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(m0-client PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::Qml
    Qt6::Quick
)

if(EMSCRIPTEN)
    # Diagnostic (firefox-on-CI reply=false, see main.cpp and README.md): the CI logs carry a
    # `PAGEERROR Aborted(). Build with -sASSERTIONS for more info.` that appears ONLY on the
    # firefox-linux runner (never on macOS firefox, chromium, or webkit), once per connection,
    # right as the socket connects. Emscripten masks the reason by default; -sASSERTIONS makes
    # the abort report what actually fired (a failed assert, an uncaught exception, a
    # pure-virtual call), which is what decides whether this abort is the root of the
    # unresolved returning-slot reply or an independent benign startup abort. Diagnostic build
    # only; drop it once the cause is known (it adds runtime checks and bundle size).
    target_link_options(m0-client PRIVATE "-sASSERTIONS=1")
endif()
