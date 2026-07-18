# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt provider library: the backend-facing family interfaces (persistence, cache,
# document) and the bundled providers, plus the QML helpers (Db, Cache, Http, Jobs) that a
# blueprint entity's Sources call. The connect point Source calls only an interface, never
# an engine; credentials live inside a provider, sourced from the entity env, never on a
# connect point or in a log. A pure service links this; the client never does.

# GLOBAL: see the note in src/service/CMakeLists.txt. Qt6::Sql is public on this target.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Sql Network Qml GLOBAL)

qt_add_library(SynQtProviders STATIC
    ipersistenceprovider.h
    icacheprovider.h
    idocumentprovider.h
    providerconfig.h
    providerregistry.cpp
    providerregistry.h
    sqliteprovider.cpp
    sqliteprovider.h
    sqlconnectionpool.cpp
    sqlconnectionpool.h
    sqlsupport.cpp
    sqlsupport.h
    postgresprovider.cpp
    postgresprovider.h
    mysqlprovider.cpp
    mysqlprovider.h
    memorycacheprovider.cpp
    memorycacheprovider.h
    memorydocumentprovider.cpp
    memorydocumentprovider.h
    persistencefactory.cpp
    persistencefactory.h
    cachefactory.cpp
    cachefactory.h
    documentfactory.cpp
    documentfactory.h
    db.cpp
    db.h
    cache.cpp
    cache.h
    http.cpp
    http.h
    jobs.cpp
    jobs.h
)

set_target_properties(SynQtProviders PROPERTIES AUTOMOC ON)

target_include_directories(SynQtProviders PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SynQtProviders PUBLIC
    Qt6::Core
    Qt6::Sql
    Qt6::Network
    Qt6::Qml
)

# External cache provider: the redis wrapper is built only when hiredis is available. It is
# an honest thin wrapper over the client library, never a reimplementation of the protocol.
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)
if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
    target_sources(SynQtProviders PRIVATE redisprovider.cpp redisprovider.h)
    target_include_directories(SynQtProviders PRIVATE "${HIREDIS_INCLUDE_DIR}")
    target_link_libraries(SynQtProviders PRIVATE "${HIREDIS_LIBRARY}")
    target_compile_definitions(SynQtProviders PRIVATE SYNQT_HAVE_HIREDIS)
    find_path(HIREDIS_SSL_INCLUDE_DIR hiredis/ssl.h)
    find_library(HIREDIS_SSL_LIBRARY hiredis_ssl)
    if(HIREDIS_SSL_INCLUDE_DIR AND HIREDIS_SSL_LIBRARY)
        target_link_libraries(SynQtProviders PRIVATE "${HIREDIS_SSL_LIBRARY}")
        target_compile_definitions(SynQtProviders PRIVATE SYNQT_HAVE_HIREDIS_SSL)
    endif()
    message(STATUS "SynQt: redis cache provider enabled (hiredis found)")
else()
    message(STATUS "SynQt: redis cache provider disabled (hiredis not found)")
endif()

# External document provider: the mongodb wrapper is built only when the MongoDB C driver is
# available (via pinned vcpkg or the system package). The driver's major versions ship
# different CMake packages and imported targets: 1.x is `mongoc-1.0` with `mongo::mongoc_shared`,
# 2.x is `mongoc` with `mongoc::shared`. Try each so either generation is picked up; the same
# `<mongoc/mongoc.h>` include and the API the provider uses are common to both.
set(_synqt_mongoc_target "")
find_package(mongoc-1.0 QUIET)
if(mongoc-1.0_FOUND)
    set(_synqt_mongoc_target mongo::mongoc_shared)
else()
    find_package(mongoc QUIET)
    if(mongoc_FOUND)
        set(_synqt_mongoc_target mongoc::shared)
    endif()
endif()
if(_synqt_mongoc_target)
    target_sources(SynQtProviders PRIVATE mongoprovider.cpp mongoprovider.h)
    target_link_libraries(SynQtProviders PRIVATE ${_synqt_mongoc_target})
    target_compile_definitions(SynQtProviders PRIVATE SYNQT_HAVE_MONGOC)
    message(STATUS "SynQt: mongodb document provider enabled (mongo-c-driver found)")
else()
    message(STATUS "SynQt: mongodb document provider disabled (mongo-c-driver not found)")
endif()
