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
# Gui is here for one call: QDesktopServices::openUrl, which is how the native desktop
# build hands a sign-in to the system browser. Both client targets link Gui anyway (a client
# renders QML), and on the WebAssembly build the object file that uses it is never pulled
# out of this static library, because nothing on that side references it.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network WebSockets Qml RemoteObjects GLOBAL)

# synqt_add_contract, for the SessionState connect point below.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

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
    browserhistory.cpp
    browserhistory.h
    clientupdate.cpp
    clientupdate.h
    devicecredential.cpp
    devicecredential.h
    securestore.cpp
    securestore.h
    nullstore.cpp
    nullstore.h
    graphics.cpp
    graphics.h
    graphicsnotice.h
    graphicsprobe.cpp
    graphicsprobe.h
    loopbackreceiver.cpp
    loopbackreceiver.h
    qmlpalette.cpp
    qmlpalette.h
    remotepageloader.cpp
    remotepageloader.h
    replicaregistry.cpp
    replicaregistry.h
    resumepath.cpp
    resumepath.h
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

# Where a native client keeps the device credential that lets a visitor stay signed in
# between launches. One store per platform, chosen here rather than at runtime, and nothing
# at all on WebAssembly (no OS store, and the browser keeps the session cookie itself).
#
# There is deliberately no fallback that writes a file. A platform with no store persists
# nothing and its visitor signs in once per launch; see src/client/nullstore.h.
if(APPLE)
    target_sources(SynQtClient PRIVATE keychainstore.cpp keychainstore.h)
    target_link_libraries(SynQtClient PRIVATE "-framework Security"
                                              "-framework CoreFoundation")
elseif(WIN32)
    target_sources(SynQtClient PRIVATE credentialmanagerstore.cpp credentialmanagerstore.h)
    target_link_libraries(SynQtClient PRIVATE Advapi32)
elseif(UNIX AND NOT EMSCRIPTEN)
    # libsecret (LGPL-2.1), the supported way in to org.freedesktop.secrets. Optional at
    # configure time: a build host without the development package still produces a working
    # client, one that does not stay signed in. It is reported either way, because "the app
    # forgets me on Linux" must be traceable to a line in a build log.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET QUIET libsecret-1)
    endif()
    if(LIBSECRET_FOUND)
        target_sources(SynQtClient PRIVATE secretservicestore.cpp secretservicestore.h)
        target_include_directories(SynQtClient SYSTEM PRIVATE ${LIBSECRET_INCLUDE_DIRS})
        target_link_libraries(SynQtClient PRIVATE ${LIBSECRET_LIBRARIES})
        target_compile_definitions(SynQtClient PRIVATE SYNQT_HAVE_LIBSECRET)
        message(STATUS "SynQt: desktop sign-in persists through the Secret Service "
                       "(libsecret ${LIBSECRET_VERSION})")
    else()
        message(STATUS "SynQt: libsecret not found, so a desktop client on this platform "
                       "will not stay signed in between launches (install libsecret-1-dev)")
    endif()
endif()

target_include_directories(SynQtClient PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The framework's own SessionState connect point, at the consumer role: the edge owns it
# (src/edge/contracts/) and this is the side that acquires it, so what the client compiles
# is a Replica and never a Source. One file, read by both libraries, because an owner and a
# consumer that disagree about a contract are a link that silently carries nothing.
#
# Built into the runtime rather than generated per app, because every
# client has a session whatever its topology says, so `Session.scope` and
# `Session.identity` must arrive in a project that declares no connect points at all.
synqt_add_contract(SynQtClient ROLE replica
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/../edge/contracts/SessionState.syn")

target_link_libraries(SynQtClient PUBLIC
    SynQtTransport
    SynQtConsumer
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::WebSockets
    Qt6::Qml
    Qt6::RemoteObjects
)
