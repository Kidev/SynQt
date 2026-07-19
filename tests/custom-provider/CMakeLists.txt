# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The custom provider fixture. The three provider sources under ${SYNQT_SCAFFOLD_DIR} are not
# in the repository: run-customprovider.sh generates them with `synqt add provider` first, so
# this target compiles the scaffolder's real output. A skeleton that stops compiling, stops
# registering, or drifts from a family interface breaks the build here.

cmake_minimum_required(VERSION 3.21)
project(SynQtCustomProviderTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Test Sql Network Qml)
qt_standard_project_setup()
enable_testing()

if(NOT DEFINED SYNQT_SCAFFOLD_DIR)
    message(FATAL_ERROR "SYNQT_SCAFFOLD_DIR must point at the generated provider sources "
                        "(run tests/custom-provider/run-customprovider.sh, not cmake directly)")
endif()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/providers" SynQtProviders)

file(GLOB SYNQT_SCAFFOLDED_PROVIDERS "${SYNQT_SCAFFOLD_DIR}/*.cpp")
list(LENGTH SYNQT_SCAFFOLDED_PROVIDERS _synqt_scaffolded_count)
if(NOT _synqt_scaffolded_count EQUAL 3)
    message(FATAL_ERROR "expected 3 scaffolded providers in ${SYNQT_SCAFFOLD_DIR}, "
                        "found ${_synqt_scaffolded_count}")
endif()

qt_add_executable(tst_customprovider
    tst_customprovider.cpp
    ${SYNQT_SCAFFOLDED_PROVIDERS}
)

target_link_libraries(tst_customprovider PRIVATE Qt6::Test SynQtProviders)

# A scaffolded provider is entity code and is held to the entity code standard: it must
# compile clean. The skeleton is the first C++ a SynQt user reads.
#
# Per compiler, because these are not a portable spelling. cl.exe parses -Wall as its own
# /Wall and then reads -Wextra as /W + "extra", which is not a number: "Command line error
# D8021: invalid numeric argument '/Wextra'". That is a hard failure before any code is
# looked at, so the Windows column was not enforcing this standard loosely; it could not
# build the fixture at all. /W4 /WX is the MSVC equivalent (/Wall there means every warning
# including the off-by-default ones, which is not what -Wall means).
if(MSVC)
    target_compile_options(tst_customprovider PRIVATE /W4 /WX)
else()
    target_compile_options(tst_customprovider PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME customprovider COMMAND tst_customprovider)
