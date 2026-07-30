# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Line coverage for the runtime libraries, off unless asked for.
#
# Only the five SynQt libraries are instrumented, never the suites that exercise them. A
# test file is not code whose coverage means anything: counting it would add thousands of
# lines that are executed by definition, and the number would climb every time a test was
# written rather than every time one reached somewhere new. What this measures is how much
# of src/ the suites reach.
#
# GCC and Clang both spell this `--coverage`, which is the compile and link flag pair
# (`-fprofile-arcs -ftest-coverage`) at once; the .gcno files land beside the objects and
# the .gcda files are written there as the tests run. Read them with
# tools/coverage/report.py, or run the whole thing with tests/run-coverage.sh.

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
        target_compile_options(${target} PRIVATE --coverage)
        # PUBLIC on the link side: the counters live in the library, but the runtime that
        # writes them out at exit (libgcov) is pulled in by whatever links it, so an
        # uninstrumented test executable still has to carry the flag through.
        target_link_options(${target} PUBLIC --coverage)
    endforeach()
endfunction()
