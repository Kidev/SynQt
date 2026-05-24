# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# One statement of how SynQt is compiled, which language it is written in, which
# diagnostics stop the build, and what a release build is allowed to do to the binary.
#
# Every CMakeLists in this repository includes this file, and `synqt build` writes the
# same include into the CMakeLists it generates for an application, so a project built
# with SynQt is held to the standard SynQt holds itself to. Include it before the first
# `add_subdirectory()` or `add_executable()` of the directory: the options below are
# directory-scoped and reach every target created afterwards, including the runtime
# libraries a standalone suite pulls in.
#
# Warnings are errors because a warning nobody has to fix is a warning nobody fixes, and
# because the three compilers here disagree about which mistakes to report. A
# narrowing conversion that breaks the Windows and macOS columns compiles silently on
# GCC. `-DSYNQT_WARNINGS_AS_ERRORS=OFF` turns the stop off for a bisect or for a new
# compiler release whose new warnings are not yet triaged. It is not meant to live in a
# preset.

include_guard(GLOBAL)

# C++20, the newest standard Qt 6.11 supports across all of its compilers. Extensions stay
# on (the default, `gnu++20`) because Qt's own headers are compiled that way and a mixed
# tree is not worth the churn.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SYNQT_WARNINGS_AS_ERRORS "Fail the build on a compiler warning" ON)
option(SYNQT_LTO "Link-time optimisation for release builds" OFF)

option(SYNQT_COMPILER_CACHE "Route the compiler through ccache/sccache when one is installed" ON)

# A compiler cache, because this tree compiles the same objects many times over. Every
# generated application add_subdirectory()s the framework from ${SYNQT_ROOT}, so one CI run
# builds SynQtService and friends about ten times. Once for the whole-tree suite, six times
# for tests/appgen-native's topologies, and once each for custom-provider, desktop-client
# and monitor-console. The redundancy is WITHIN one run, so this pays off on a cold cache
# too. A cache restored between runs is a bonus on top of that, not the mechanism.
#
# sccache on MSVC and ccache elsewhere. Ccache does not handle MSVC's /Zi debug format, and
# sccache is the build that does. Silent when neither is installed, because a
# message(WARNING) here would fail tests/run-all.sh, which treats a CMake warning as a
# defect.
if(SYNQT_COMPILER_CACHE AND NOT CMAKE_C_COMPILER_LAUNCHER AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    if(MSVC)
        find_program(SYNQT_CACHE_PROGRAM sccache)
    else()
        find_program(SYNQT_CACHE_PROGRAM ccache)
    endif()
    if(SYNQT_CACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${SYNQT_CACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SYNQT_CACHE_PROGRAM}")
        # sccache cannot cache a separate .pdb, so ask MSVC to embed debug info instead.
        # Without this every compile is a miss and the cache is pure overhead, and it is
        # worse than that. Sccache runs cl.exe detached from the mspdbsrv.exe instance that
        # coordinates concurrent writers of one .pdb, so /FS does not save them and every
        # parallel compile into the same target directory dies with
        #
        #   fatal error C1041: cannot open program database '...pdb';
        #   if multiple CL.EXE write to the same .PDB file, please use /FS
        #
        # which is what took out both Windows columns of the ctest workflow.
        #
        # Said twice, because there are two mechanisms and which one is live depends on a
        # policy this tree does not set. CMAKE_MSVC_DEBUG_INFORMATION_FORMAT is the
        # abstraction, and it is read only under CMP0141=NEW; below that policy (which is
        # where `cmake_minimum_required(VERSION 3.21)` leaves this tree, since CMP0141 arrived in
        # 3.25) CMake ignores it completely and the debug format is whatever /Zi sits in the
        # per-configuration flags. So the abstraction is set for the day the floor rises,
        # and the flags it would generate are rewritten for today. Both say /Z7.
        if(MSVC)
            set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
            foreach(configuration DEBUG RELWITHDEBINFO RELEASE MINSIZEREL)
                foreach(language C CXX)
                    string(REPLACE "/Zi" "/Z7"
                           "CMAKE_${language}_FLAGS_${configuration}"
                           "${CMAKE_${language}_FLAGS_${configuration}}")
                endforeach()
            endforeach()
            add_link_options(/DEBUG:NONE)
        endif()
        message(STATUS "SynQt: compiling through ${SYNQT_CACHE_PROGRAM}")
    endif()
endif()

option(SYNQT_STRIP "Leave no symbols in the linked binaries" OFF)
option(SYNQT_DEV_TOOLS "Compile the development-only sources into the framework" OFF)

# Stripping, because a release artifact has no use for a symbol table and every reason not
# to ship one: it is the map an attacker reads first, and on the WebAssembly client it is
# bytes every visitor downloads. `synqt build --release` turns this on. Nothing else does.
#
# Not a build type. CMake's `Release` and `MinSizeRel` both still emit a symbol table and
# CMake has no portable setting for this, so each branch below is the flag that linker
# takes rather than one spelling hopefully understood by all of them.
if(SYNQT_STRIP)
    if(MSVC)
        # MSVC keeps debug information in a separate .pdb and the linker writes one only when
        # asked, so the whole of stripping here is not asking. /OPT:REF and /OPT:ICF are
        # already on for the release configurations below.
        add_link_options(/DEBUG:NONE)
    elseif(EMSCRIPTEN)
        # The name section is what matters in a .wasm. Without --strip-all it carries every
        # function's name, which is both the map and the bytes. ASSERTIONS is already off at
        # -O1 and above. Saying so explicitly makes a `--custom Debug --strip` build say
        # what it is.
        add_link_options(-sASSERTIONS=0)
        add_link_options("SHELL:-Wl,--strip-all")
    elseif(APPLE)
        # ld64 has no --strip-all. -x drops the local symbols and -S the debug map, which is
        # what `strip -x -S` does and as far as a linked Mach-O goes without breaking dynamic
        # linking.
        add_link_options("SHELL:-Wl,-x" "SHELL:-Wl,-S")
    else()
        add_link_options("SHELL:-Wl,--strip-all")
    endif()
endif()

# The development-only sources. The stub identity provider, and every other capability that
# must not exist in a shipped artifact. OFF by default, so a bare `cmake` produces a
# production-shaped build and only `synqt dev` (with the suites that test those sources)
# turns it on.
#
# It gates the source list rather than a runtime branch, and that is the entire point. A
# capability behind an `if (devMode)` is still in the binary. It can be reached through a bug
# in the check, through a flag someone passes, or read out of the strings. A file
# CMake never names is not compiled, not linked, and not there. The definition below is what
# lets a development-only header refuse to be included in a build that did not ask for one.
# See src/edge/CMakeLists.txt for the list, and docs/security.md for what this defends.
if(SYNQT_DEV_TOOLS)
    add_compile_definitions(SYNQT_DEV_TOOLS)
endif()

# MSVC is true for clang-cl as well, and this branch relies on that. The Windows gate under
# tools/windows-check drives clang-cl, and it has to be told about the same warnings in
# the same spelling as cl.exe, not in GCC's.
if(MSVC)
    # /W4 is the highest level that is about the code rather than about the standard
    # library's own headers (/Wall reports thousands of them). /permissive- turns off the
    # last of the pre-standard MSVC dialect, so a construct that compiles here compiles
    # with GCC and Clang too. /utf-8 makes the source encoding explicit. Without it MSVC
    # reads the file in the machine's active code page and a non-ASCII byte in a comment
    # can end a line early.
    add_compile_options(/W4 /permissive- /utf-8)
    # C4702 (unreachable code) is the one warning in /W4 that cannot be acted on here. MSVC
    # emits it from the optimiser, after inlining, so the line it names is inside whichever
    # header the inlined body came from. With /O2 the current Qt headers produce it from
    # qmetatype.h, qvariant.h and qjsengine.h, in translation units that only include them.
    # `/external:W0` does not reach it either, because /external is a front-end facility and
    # this warning is raised by the back end, after the include context is gone. GCC and
    # Clang have no equivalent in -Wall -Wextra, so leaving it on buys no coverage on the
    # other two columns and reddens this one whenever a new MSVC inlines differently.
    add_compile_options(/wd4702)
    if(SYNQT_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra)
    if(SYNQT_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# Release: keep only what is reachable.
#
# CMake already supplies the optimisation level itself (`/O2 /Ob2 /DNDEBUG` for MSVC,
# `-O3 -DNDEBUG` for GCC and Clang), so what is added here is the part CMake does not do:
# emitting each function and each variable into its own section so the linker can drop the
# ones nothing calls. On a binary that links Qt statically (every WebAssembly client)
# that is the difference between shipping the modules used and shipping the modules linked.
#
# Link-time optimisation is not added. It sits behind SYNQT_LTO and is off. It costs
# minutes per link, and Qt's static plugin registration relies on constructors in
# translation units nothing references, which is exactly what an aggressive LTO pass is
# built to remove. Turn it on for a measured release, not for a working tree.
if(MSVC)
    add_compile_options($<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:/Gy>
                        $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:/Gw>)
    add_link_options($<$<CONFIG:Release,MinSizeRel>:/OPT:REF>
                     $<$<CONFIG:Release,MinSizeRel>:/OPT:ICF>)
elseif(NOT EMSCRIPTEN)
    # Emscripten is left out because wasm-ld already drops unreferenced functions, and
    # `--gc-sections` is not a flag it takes.
    add_compile_options($<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:-ffunction-sections>
                        $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:-fdata-sections>)
    if(APPLE)
        add_link_options($<$<CONFIG:Release,MinSizeRel>:-Wl,-dead_strip>)
    else()
        add_link_options($<$<CONFIG:Release,MinSizeRel>:-Wl,--gc-sections>)
    endif()
endif()

if(SYNQT_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT synqt_ipo_supported OUTPUT synqt_ipo_reason)
    if(synqt_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    else()
        message(WARNING "SYNQT_LTO asked for, but this toolchain refuses it: "
                        "${synqt_ipo_reason}")
    endif()
endif()
