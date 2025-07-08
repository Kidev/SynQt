# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Cross-compile the SynQt native targets for the Windows MSVC ABI from a Linux host,
# using clang-cl against the Microsoft CRT and Windows SDK that `xwin splat` fetches.
# A compile-and-ABI gate: clang-cl's warnings differ from cl.exe's, and nothing runs.
#
# Driven by tools/windows-check/check-windows.sh, which sets the two paths this needs:
#   XWIN_DIR   the `xwin splat` output (its crt/ and sdk/ subtrees)
#   QT_HOST_PATH (passed as a normal CMake var) the Linux host Qt kit that provides
#                the code generators (moc, rcc, uic, repc) as native executables, since
#                the Windows kit's are .exe and cannot run here.
# CMAKE_PREFIX_PATH points at the Windows Qt kit (aqt win64_msvc2022_64).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED ENV{XWIN_DIR})
    message(FATAL_ERROR
        "windows-clang-cl.cmake: set the XWIN_DIR environment variable to the "
        "`xwin splat` output directory (the one holding crt/ and sdk/).")
endif()
file(TO_CMAKE_PATH "$ENV{XWIN_DIR}" _xwin)

if(NOT EXISTS "${_xwin}/crt/include")
    message(FATAL_ERROR
        "windows-clang-cl.cmake: XWIN_DIR='${_xwin}' has no crt/include; run "
        "`xwin splat` into it first (see tools/windows-check/README.md).")
endif()

# The release dynamic CRT in every configuration: xwin ships no msvcrtd.lib, and the Qt
# kit links the release CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# clang-cl defaults to the x86_64-pc-windows-msvc target, but pin it so the toolchain
# is explicit regardless of the driver's host default.
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# The LLVM Windows-flavoured binutils. lld-link is the linker. Llvm-lib and llvm-rc
# stand in for lib.exe and rc.exe.
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_RC_COMPILER_INIT llvm-rc)

# The MSVC CRT and Windows SDK headers, handed to clang-cl as system includes (/imsvc)
# so their own warnings never trip a -Werror build of SynQt's code. Ordering mirrors what a
# real vcvars environment puts in INCLUDE.
set(_win_includes
    "/imsvc${_xwin}/crt/include"
    "/imsvc${_xwin}/sdk/include/ucrt"
    "/imsvc${_xwin}/sdk/include/um"
    "/imsvc${_xwin}/sdk/include/shared"
    "/imsvc${_xwin}/sdk/include/winrt"
    "/imsvc${_xwin}/sdk/include/cppwinrt")
string(JOIN " " _win_include_flags ${_win_includes})

# -fuse-ld=lld makes clang-cl drive lld-link. /EHsc is the usual MSVC exception model
# clang-cl expects to be told explicitly.
set(_common_flags "-fuse-ld=lld /EHsc ${_win_include_flags}")
set(CMAKE_C_FLAGS_INIT "${_common_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_common_flags}")

# The import libraries, in the lowercase-arch layout `xwin splat` emits by default.
set(_win_libpaths
    "/libpath:${_xwin}/crt/lib/x86_64"
    "/libpath:${_xwin}/sdk/lib/ucrt/x86_64"
    "/libpath:${_xwin}/sdk/lib/um/x86_64")
string(JOIN " " _win_libpath_flags ${_win_libpaths})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_win_libpath_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_win_libpath_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_win_libpath_flags}")

# Programs are always the host's: the code generators (moc, rcc, repc) come from
# QT_HOST_PATH, never the Windows kit whose .exe generators cannot run here.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# No single sysroot, so search everywhere; the .lib suffix keeps host libraries out.
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
