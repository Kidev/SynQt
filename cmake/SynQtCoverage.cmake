# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Line coverage for the runtime libraries. Off unless asked for. Read the counters with
# tools/coverage/report.py, or run tests/run-coverage.sh.

option(SYNQT_COVERAGE "Instrument the SynQt runtime libraries for line coverage" OFF)

function(synqt_enable_coverage)
    if(NOT SYNQT_COVERAGE)
        return()
    endif()
    if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        message(FATAL_ERROR
            "SYNQT_COVERAGE needs GCC or Clang; this tree is configured with "
            "${CMAKE_CXX_COMPILER_ID}.")
    endif()
    foreach(target IN LISTS ARGV)
        if(NOT TARGET ${target})
            message(FATAL_ERROR "SYNQT_COVERAGE: no target named ${target}")
        endif()
        # PUBLIC, so every unit that links a library is instrumented too. An inline function
        # in a library header is compiled into each unit that calls it and the linker keeps
        # one copy, often a suite's, which then takes the library's calls as well. The report
        # still counts src/ only.
        target_compile_options(${target} PUBLIC --coverage)
        # The executable that links the library must pull in libgcov.
        target_link_options(${target} PUBLIC --coverage)
    endforeach()
endfunction()
