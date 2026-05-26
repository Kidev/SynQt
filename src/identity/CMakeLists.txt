# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt login stack: the OAuth2/OIDC engine that holds the client secret and the tokens
# (OAuthBackend, EdgeReplyHandler), the ID-token signature check against the provider JWKS
# (JwksVerifier), and the Identity and SessionStore connect points a dedicated auth entity
# owns (`identity.provider_entity`, docs/authentication.md "Where identity runs").
#
# Its own library because Qt Network Authorization is GPLv3-only: an entity that links this
# is GPLv3, and one that does not is not. Only the web edge (through ../edge) and the auth
# entity link it. The HTTP routes that drive the flow are the edge's, not this library's,
# so the auth entity gets the engine without ever linking Qt HTTP Server.

# GLOBAL: see the note in ../service/CMakeLists.txt.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Qml RemoteObjects NetworkAuth GLOBAL)

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

if(NOT TARGET SynQtService)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../service" "${CMAKE_BINARY_DIR}/SynQtService")
endif()

# synqt_add_contract, for the two connect points below: each is an ordinary contract owner,
# generated the same way an app's connect points are.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtContracts.cmake")

qt_add_library(SynQtIdentity STATIC
    claimstore.cpp
    claimstore.h
    identityconfig.h
    deviceregistry.cpp
    deviceregistry.h
    edgereplyhandler.cpp
    edgereplyhandler.h
    jwksverifier.cpp
    jwksverifier.h
    oauthbackend.cpp
    oauthbackend.h
    identityservice.cpp
    identityservice.h
)

set_target_properties(SynQtIdentity PROPERTIES AUTOMOC ON)

target_include_directories(SynQtIdentity PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# SYSTEM: jwt-cpp and the picojson it vendors are not ours to keep warning-clean, and
# this tree compiles with -Werror. Qt's own headers arrive the same way. PUBLIC because
# the dev stub identity provider in ../edge signs its ID tokens with the same header-only
# library, and this is the target that knows where it was found.
target_include_directories(SynQtIdentity SYSTEM PUBLIC "${JWT_CPP_INCLUDE_DIR}")

# Identity and SessionStore are the auth entity's two connect points, owned by the entity
# `identity.provider_entity` names. They are framework contracts, not app contracts: no
# `synqt.yaml` declares them and no app carries an `Identity.syn`, so the auth entity's
# generated main calls the Source registration from this library rather than compiling its
# own copy. That is what makes promoting identity to its own entity the one-line
# configuration change the docs promise. The generated headers are exposed PUBLIC (unlike a
# normal per-executable synqt_add_contract call) so a consumer needing one finds it without
# generating and linking its own duplicate copy alongside this static library's.
synqt_add_contract(SynQtIdentity ROLE source
    SYN "${CMAKE_CURRENT_SOURCE_DIR}/contracts/Identity.syn"
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/SessionStore.syn")
target_include_directories(SynQtIdentity PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/synqt_generated/SynQtIdentity"
    "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(SynQtIdentity PUBLIC
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    Qt6::NetworkAuth
)
