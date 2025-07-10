# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt service runtime library, linked into every native service entity. It
# provides the mesh transport (mutual TLS by default, opt-in local socket), the entity
# runtime and connect-point host (topology resolution, deny by default), the web edge
# (bundle serving, header policy, the WebSocket upgrade pipeline), and loads each owned
# Source from the entity's QML. Later milestones add sessions and the Caller accessor.

# GLOBAL, as in src/client and src/consumer: this library is pulled in with add_subdirectory and
# publicly links these modules, but find_package's imported targets are directory-scoped by
# default. Without it the consumer's executable is finalized in a scope where Qt6::NetworkAuth and
# friends do not exist, and Qt warns "The NetworkAuth target is mentioned as a dependency for
# SynQtService, but not declared. The linking might be incomplete." once per target.
find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core Network NetworkAuth Qml RemoteObjects WebSockets HttpServer GLOBAL)
find_package(OpenSSL REQUIRED)

# jwt-cpp (MIT, header-only) for ID-token signature verification. Pinned via vcpkg; the
# CLI resolves the toolchain (M10). Until then, locate the installed header, with a hint
# to the local vcpkg tree, overridable via -DJWT_CPP_INCLUDE_DIR=... or the environment
# variable of the same name (which is how CI points at its own checkout: the implicit
# system include prefixes are not searched the same way on every platform).
find_path(JWT_CPP_INCLUDE_DIR jwt-cpp/jwt.h
    HINTS "$ENV{JWT_CPP_INCLUDE_DIR}"
          "$ENV{HOME}/.local/bin/vcpkg/installed/x64-linux/include"
          "$ENV{VCPKG_ROOT}/installed/x64-linux/include")
if(NOT JWT_CPP_INCLUDE_DIR)
    message(FATAL_ERROR "jwt-cpp not found. Install it (vcpkg install jwt-cpp) or set JWT_CPP_INCLUDE_DIR.")
endif()

# jwt-cpp ships no version macro, so require the symbol JwksVerifier actually calls rather than
# a version number: create_public_key_from_rsa_components (it builds the RSA public key from the
# JWK modulus/exponent) arrived in v0.7.1. Against an older header the build dies ~200 lines deep
# in std::string overload candidates, naming basic_string rather than jwt-cpp, so say it here,
# at configure time, in terms of the thing to go fix.
file(STRINGS "${JWT_CPP_INCLUDE_DIR}/jwt-cpp/jwt.h" JWT_CPP_HAS_RSA_COMPONENTS
    REGEX "create_public_key_from_rsa_components" LIMIT_COUNT 1)
if(NOT JWT_CPP_HAS_RSA_COMPONENTS)
    message(FATAL_ERROR
        "jwt-cpp at ${JWT_CPP_INCLUDE_DIR} is too old: SynQt needs v0.7.1 or newer "
        "(jwt::helper::create_public_key_from_rsa_components, used by JwksVerifier).")
endif()

# The shared transport adapter (guarded so several entities can pull it in once).
if(NOT TARGET SynQtTransport)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../transport" "${CMAKE_BINARY_DIR}/SynQtTransport")
endif()

# The shared consumer surface (facade base, connect-point resolver, Promise): a service that
# consumes another entity's connect point reaches it through the generated facade.
if(NOT TARGET SynQtConsumer)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../consumer" "${CMAKE_BINARY_DIR}/SynQtConsumer")
endif()

# The provider layer (family interfaces, bundled providers, Db/Cache/Http/Jobs helpers).
# EntityRuntime injects the blueprint's helper into each owned Source; a service links this,
# the client never does.
if(NOT TARGET SynQtProviders)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../providers" "${CMAKE_BINARY_DIR}/SynQtProviders")
endif()

qt_add_library(SynQtService STATIC
    meshpeer.h
    meshserver.cpp
    meshserver.h
    meshclient.cpp
    meshclient.h
    topology.cpp
    topology.h
    connectpointhost.cpp
    connectpointhost.h
    entityruntime.cpp
    entityruntime.h
    sessionmanager.cpp
    sessionmanager.h
    caller.cpp
    caller.h
    identityconfig.h
    identitymapping.h
    edgereplyhandler.cpp
    edgereplyhandler.h
    oauthbackend.cpp
    oauthbackend.h
    identityprovider.cpp
    identityprovider.h
    identityservice.cpp
    identityservice.h
    jwksverifier.cpp
    jwksverifier.h
    stubidentityserver.cpp
    stubidentityserver.h
    webedgeconfig.h
    webedge.cpp
    webedge.h
)

set_target_properties(SynQtService PROPERTIES AUTOMOC ON)

target_include_directories(SynQtService PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(SynQtService PRIVATE "${JWT_CPP_INCLUDE_DIR}")

target_link_libraries(SynQtService PUBLIC
    SynQtTransport
    SynQtConsumer
    SynQtProviders
    Qt6::Core
    Qt6::Network
    Qt6::NetworkAuth
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::WebSockets
    Qt6::HttpServer
    OpenSSL::Crypto
)
