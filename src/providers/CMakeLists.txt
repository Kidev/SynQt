# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt provider library: the backend-facing family interfaces (persistence, cache,
# document) and the bundled providers, plus the QML helpers (Db, Cache, Docs, Http, Jobs)
# an entity's Sources call. The connect point Source calls only an interface, never
# an engine; credentials live inside a provider, sourced from the entity env, never on a
# connect point or in a log. A pure service links this; the client never does.

# GLOBAL: see the note in src/service/CMakeLists.txt. Qt6::Sql is public on this target.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Sql Network Qml GLOBAL)

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
    pooledsqlprovider.cpp
    pooledsqlprovider.h
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
    docs.cpp
    docs.h
    http.cpp
    http.h
    jobs.cpp
    jobs.h
)

set_target_properties(SynQtProviders PROPERTIES AUTOMOC ON)

target_include_directories(SynQtProviders PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# SynQtConsumer for the Promise the Http helper answers with, the same one a returning slot
# answers with.
if(NOT TARGET SynQtConsumer)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../consumer" "${CMAKE_BINARY_DIR}/SynQtConsumer")
endif()

target_link_libraries(SynQtProviders PUBLIC
    Qt6::Core
    Qt6::Sql
    Qt6::Network
    Qt6::Qml
    SynQtConsumer
)

# The redis wrapper, built only when hiredis is available. External libraries are recorded
# for THIRD-PARTY-LICENSES (licenses.provider_libraries).
set(_synqt_provider_libraries "")

find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)
if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
    target_sources(SynQtProviders PRIVATE redisprovider.cpp redisprovider.h)
    target_include_directories(SynQtProviders PRIVATE "${HIREDIS_INCLUDE_DIR}")
    target_link_libraries(SynQtProviders PRIVATE "${HIREDIS_LIBRARY}")
    target_compile_definitions(SynQtProviders PRIVATE SYNQT_HAVE_HIREDIS)
    list(APPEND _synqt_provider_libraries hiredis)
    # OpenSSL is linked directly as well as through hiredis_ssl: the provider builds its own
    # context to check the server's name, which hiredis does not (see
    # RedisCacheProvider::startTls).
    find_path(HIREDIS_SSL_INCLUDE_DIR hiredis/hiredis_ssl.h)
    find_library(HIREDIS_SSL_LIBRARY hiredis_ssl)
    find_package(OpenSSL QUIET)
    if(HIREDIS_SSL_INCLUDE_DIR AND HIREDIS_SSL_LIBRARY AND OpenSSL_FOUND)
        target_link_libraries(SynQtProviders PRIVATE
            "${HIREDIS_SSL_LIBRARY}" OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(SynQtProviders PRIVATE SYNQT_HAVE_HIREDIS_SSL)
        list(APPEND _synqt_provider_libraries OpenSSL)
        message(STATUS "SynQt: redis TLS enabled (hiredis_ssl and OpenSSL found)")
    else()
        message(STATUS "SynQt: redis TLS disabled (hiredis_ssl or OpenSSL not found); "
                       "a provider with tls: true refuses to connect")
    endif()
    message(STATUS "SynQt: redis cache provider enabled (hiredis found)")
else()
    message(STATUS "SynQt: redis cache provider disabled (hiredis not found)")
endif()

# The mongodb wrapper, built only when the MongoDB C driver is available: `mongoc-1.0` for
# 1.x, `mongoc` for 2.x.
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
    list(APPEND _synqt_provider_libraries mongoc)
    message(STATUS "SynQt: mongodb document provider enabled (mongo-c-driver found)")
else()
    message(STATUS "SynQt: mongodb document provider disabled (mongo-c-driver not found)")
endif()

list(JOIN _synqt_provider_libraries "\n" _synqt_provider_libraries_text)
file(WRITE "${CMAKE_BINARY_DIR}/synqt-provider-libraries.txt" "${_synqt_provider_libraries_text}\n")
