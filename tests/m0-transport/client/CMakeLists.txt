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

# The application-side mitigation for the posted-event stall, off by default so the spike keeps
# measuring what a plain Qt client does. See main.cpp, and qt-patches/README.md for the same
# fix made inside Qt instead. Turn it on with -DM0_POSTED_EVENT_PUMP=ON to run the comparison.
option(M0_POSTED_EVENT_PUMP "Sweep the posted event queue from a QTimer (WASM mitigation)" OFF)
if(M0_POSTED_EVENT_PUMP)
    target_compile_definitions(m0-client PRIVATE M0_POSTED_EVENT_PUMP)
endif()

# The other application-side mitigation, and the only one that needs no change to Qt and no
# change to the application: link with asyncify. QEventDispatcherWasm has two entirely
# different shapes depending on qstdweb::haveAsyncify(), which is a runtime probe of the
# Emscripten runtime (`typeof Asyncify !== "undefined"`), not a Qt build option, so an
# application can select the other shape on its own. Without asyncify the main thread cannot
# block, so exec() returns to the browser and processEvents() runs only when the wakeup timer
# fires. With asyncify the main thread suspends inside processEvents() and any registered
# event handler at all resumes it, after which sendAllEvents() sweeps the posted queue again.
# That gives posted events a second delivery path that no single dropped callback can take
# away. Measured with verify/verify-pump.mjs; see qt-patches/README.md.
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
    # Diagnostic (firefox-on-CI reply=false, see main.cpp and README.md): the CI logs carry a
    # `PAGEERROR Aborted(). Build with -sASSERTIONS for more info.` that appears ONLY on the
    # firefox-linux runner (never on macOS firefox, chromium, or webkit), once per connection,
    # right as the socket connects. Emscripten masks the reason by default; -sASSERTIONS makes
    # the abort report what actually fired (a failed assert, an uncaught exception, a
    # pure-virtual call), which is what decides whether this abort is the root of the
    # unresolved returning-slot reply or an independent benign startup abort. Diagnostic build
    # only; drop it once the cause is known (it adds runtime checks and bundle size).
    # -lembind for the URL read in main.cpp, and DYNAMIC_EXECUTION=0 to drop the eval-based
    # runtime entirely, which is what the generated client links with (cmakegen.py). Without
    # it the bundle needs `script-src 'unsafe-eval'` and the spike could not be served under
    # the policy the edge actually emits, so the multi-threaded proof would be measuring a
    # page no SynQt app resembles.
    target_link_options(m0-client PRIVATE "-sASSERTIONS=1" "-lembind" "-sDYNAMIC_EXECUTION=0")

    if(M0_ASYNCIFY)
        # -Os is Emscripten's own recommendation for asyncify builds, and the stack size is
        # raised because the suspend point sits under the whole Qt event-dispatch stack, which
        # is far deeper than the 4 KB default asyncify reserves.
        target_link_options(m0-client PRIVATE
            "-sASYNCIFY" "-sASYNCIFY_STACK_SIZE=131072" "-Os")
    endif()
endif()
