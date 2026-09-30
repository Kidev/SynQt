# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The custom providers the advanced tutorials build, compiled from the pages themselves and
# linked against the real provider library.

cmake_minimum_required(VERSION 3.21)
project(synqt_docs_providers LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SynQtBuildFlags.cmake")

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Sql Test)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
qt_standard_project_setup()

if(NOT TARGET SynQtProviders)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/providers" "${CMAKE_BINARY_DIR}/SynQtProviders")
endif()

enable_testing()

set(_docs "${CMAKE_CURRENT_SOURCE_DIR}/../../docs")
set(_extract "${CMAKE_CURRENT_SOURCE_DIR}/extract.py")
set(_pages
    "tutorial-advanced-cache.md|memcachedprovider.cpp"
    "tutorial-advanced-database.md|sqlserverprovider.cpp"
)
set(_extracted "")
foreach(_pair IN LISTS _pages)
    string(REPLACE "|" ";" _pair "${_pair}")
    list(GET _pair 0 _page)
    list(GET _pair 1 _file)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_file}"
        COMMAND "${Python3_EXECUTABLE}" "${_extract}" "${_docs}/${_page}"
                "${CMAKE_CURRENT_BINARY_DIR}/${_file}"
        DEPENDS "${_extract}" "${_docs}/${_page}"
        VERBATIM)
    list(APPEND _extracted "${CMAKE_CURRENT_BINARY_DIR}/${_file}")
endforeach()

qt_add_executable(tst_docsproviders tst_docsproviders.cpp ${_extracted})
target_link_libraries(tst_docsproviders PRIVATE
    SynQtProviders
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)

add_test(NAME docs-providers COMMAND tst_docsproviders)
