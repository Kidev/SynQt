# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# One statement of how SynQt is compiled: which language it is written in, which
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
# because the three compilers here disagree about which mistakes are worth mentioning:
# the narrowing conversion that broke the Windows and macOS columns compiled silently on
# GCC. `-DSYNQT_WARNINGS_AS_ERRORS=OFF` turns the stop off for a bisect or for a new
# compiler release whose new warnings are not yet triaged; it is not meant to live in a
# preset.

include_guard(GLOBAL)

# C++20, the newest standard Qt 6.11 supports across all of its compilers. Extensions stay
# on (the default, `gnu++20`) because Qt's own headers are compiled that way and a mixed
# tree is not worth the churn.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SYNQT_WARNINGS_AS_ERRORS "Fail the build on a compiler warning" ON)
option(SYNQT_LTO "Link-time optimisation for release builds" OFF)

# MSVC is true for clang-cl as well, which is the point: the Windows gate under
# tools/windows-check drives clang-cl, and it has to be told about the same warnings in
# the same spelling as cl.exe, not in GCC's.
if(MSVC)
    # /W4 is the highest level that is about the code rather than about the standard
    # library's own headers (/Wall reports thousands of them). /permissive- turns off the
    # last of the pre-standard MSVC dialect, so a construct that compiles here compiles
    # with GCC and Clang too. /utf-8 makes the source encoding explicit; without it MSVC
    # reads the file in the machine's active code page and a non-ASCII byte in a comment
    # can end a line early.
    add_compile_options(/W4 /permissive- /utf-8)
    # C4702 (unreachable code) is the one warning in /W4 that cannot be acted on here. MSVC
    # emits it from the optimiser, after inlining, so the line it names is inside whichever
    # header the inlined body came from: with /O2 the current Qt headers produce it from
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
# ones nothing calls. On a binary that links Qt statically -- every WebAssembly client --
# that is the difference between shipping the modules used and shipping the modules linked.
#
# Not added, deliberately: link-time optimisation, behind SYNQT_LTO and off. It costs
# minutes per link, and Qt's static plugin registration relies on constructors in
# translation units nothing references, which is exactly what an aggressive LTO pass is
# built to remove. Turn it on for a measured release, not for a working tree.
if(MSVC)
    add_compile_options($<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:/Gy>
                        $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:/Gw>)
    add_link_options($<$<CONFIG:Release,MinSizeRel>:/OPT:REF>
                     $<$<CONFIG:Release,MinSizeRel>:/OPT:ICF>)
elseif(NOT EMSCRIPTEN)
    # Emscripten is left out on purpose: wasm-ld already drops unreferenced functions, and
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
