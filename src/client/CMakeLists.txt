# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt client runtime library. It links into the WebAssembly client and the native
# desktop client (the same QML), and holds no secret and no mesh certificate. It reaches
# services only through the edge. Qt Core + Network + WebSockets + Qml + RemoteObjects
# here; never HttpServer, NetworkAuth, or Sql (the client never listens, holds secrets,
# or touches storage).

# GLOBAL: see the note in src/service/CMakeLists.txt. Gui is for QDesktopServices::openUrl
# in the desktop sign-in.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Network WebSockets Qml RemoteObjects GLOBAL)

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
    privacy.cpp
    privacy.h
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

# The device credential store, one per platform, none on WebAssembly. No file fallback
# (src/client/nullstore.h).
if(APPLE)
    target_sources(SynQtClient PRIVATE keychainstore.cpp keychainstore.h)
    target_link_libraries(SynQtClient PRIVATE "-framework Security"
                                              "-framework CoreFoundation")
elseif(WIN32)
    target_sources(SynQtClient PRIVATE credentialmanagerstore.cpp credentialmanagerstore.h)
    target_link_libraries(SynQtClient PRIVATE Advapi32)
elseif(UNIX AND NOT EMSCRIPTEN)
    # libsecret (LGPL-2.1). Optional; without it the client does not stay signed in.
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

# The privacy components. Keep this prefix in step with the URLs in registerPrivacyTypes().
qt_add_resources(SynQtClient "synqt_privacy_qml"
    PREFIX "/qt/qml/SynQt/privacy"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/privacy"
    FILES
        privacy/LegalFooter.qml
        privacy/CookieConsent.qml
        privacy/DataErasureRequest.qml
)

# The SessionState replica. The contract file is shared with src/edge, which owns it.
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
