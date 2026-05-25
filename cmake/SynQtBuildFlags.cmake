# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# How SynQt and every generated application compile. Include it before the directory's
# first add_subdirectory() or add_executable(): the options are directory-scoped.

include_guard(GLOBAL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SYNQT_WARNINGS_AS_ERRORS "Fail the build on a compiler warning" ON)
option(SYNQT_LTO "Link-time optimisation for release builds" OFF)

option(SYNQT_COMPILER_CACHE "Route the compiler through ccache/sccache when one is installed" ON)

# sccache on MSVC, ccache elsewhere. Silent when neither is installed: tests/run-all.sh
# fails on any CMake warning.
if(SYNQT_COMPILER_CACHE AND NOT CMAKE_C_COMPILER_LAUNCHER AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    if(MSVC)
        find_program(SYNQT_CACHE_PROGRAM sccache)
    else()
        find_program(SYNQT_CACHE_PROGRAM ccache)
    endif()
    if(SYNQT_CACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${SYNQT_CACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SYNQT_CACHE_PROGRAM}")
        # sccache cannot share a .pdb between parallel compiles, so embed debug info (/Z7).
        # The variable needs CMP0141=NEW, which the 3.21 floor does not set, so the flags
        # are rewritten too.
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

# Set by `synqt build --release`. One branch per linker: CMake has no portable setting.
if(SYNQT_STRIP)
    if(MSVC)
        add_link_options(/DEBUG:NONE)
    elseif(EMSCRIPTEN)
        add_link_options(-sASSERTIONS=0)
        add_link_options("SHELL:-Wl,--strip-all")
    elseif(APPLE)
        # ld64 has no --strip-all.
        add_link_options("SHELL:-Wl,-x" "SHELL:-Wl,-S")
    else()
        add_link_options("SHELL:-Wl,--strip-all")
    endif()
endif()

# Gates the development-only source files, not a runtime branch (docs/security.md).
# The definition lets a development-only header refuse any other build.
if(SYNQT_DEV_TOOLS)
    add_compile_definitions(SYNQT_DEV_TOOLS)
endif()

# MSVC is also true for clang-cl, which tools/windows-check relies on.
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    # C4702 fires inside inlined Qt headers, where /external:W0 cannot reach.
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

# Release keeps only reachable code. LTO stays off by default: it can drop Qt's static
# plugin registration constructors.
if(MSVC)
    add_compile_options($<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:/Gy>
                        $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:/Gw>)
    add_link_options($<$<CONFIG:Release,MinSizeRel>:/OPT:REF>
                     $<$<CONFIG:Release,MinSizeRel>:/OPT:ICF>)
elseif(NOT EMSCRIPTEN)
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
