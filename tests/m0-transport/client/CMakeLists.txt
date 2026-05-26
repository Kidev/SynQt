# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network RemoteObjects WebSockets Qml Quick)
qt_standard_project_setup(REQUIRES 6.12)

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

# The application-side mitigation for the posted-event stall, off by default
# (-DM0_POSTED_EVENT_PUMP=ON). See main.cpp and qt-patches/README.md.
option(M0_POSTED_EVENT_PUMP "Sweep the posted event queue from a QTimer (WASM mitigation)" OFF)
if(M0_POSTED_EVENT_PUMP)
    target_compile_definitions(m0-client PRIVATE M0_POSTED_EVENT_PUMP)
endif()

# The other mitigation: link with asyncify. QEventDispatcherWasm probes for asyncify at run
# time, and with it any browser event resumes processEvents() and drains the posted queue.
# See qt-patches/README.md.
option(M0_ASYNCIFY "Link the client with asyncify (WASM mitigation)" OFF)

target_link_libraries(m0-client PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::Qml
    Qt6::Quick
)

if(EMSCRIPTEN)
    # -sASSERTIONS names the abort seen on the firefox-linux runner (see README.md). -lembind
    # for the URL read in main.cpp; DYNAMIC_EXECUTION=0, as the generated client links, so the
    # spike runs under the edge's CSP.
    target_link_options(m0-client PRIVATE "-sASSERTIONS=1" "-lembind" "-sDYNAMIC_EXECUTION=0")

    if(M0_ASYNCIFY)
        # -Os and a larger stack: the suspend point sits under the whole event-dispatch stack.
        target_link_options(m0-client PRIVATE
            "-sASYNCIFY" "-sASYNCIFY_STACK_SIZE=131072" "-Os")
    endif()
endif()
