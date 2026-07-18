# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Cross-compile the SynQt native targets for the Windows MSVC ABI from a Linux host,
# using clang-cl against the Microsoft CRT and Windows SDK that `xwin splat` fetches.
# This exists to catch the "does the Windows code path even compile" class of failure
# locally, before a push spends a CI round trip on it: the Q_OS_WIN blocks, the MSVC
# STL and SDK headers, the named-pipe and ACL calls, the types and includes that only
# exist or only differ on Windows.
#
# Honest scope: clang-cl targets the same ABI as cl.exe and consumes the same headers,
# so a target that compiles here compiles on the CI's MSVC. It is NOT a byte-for-byte
# replica of cl.exe /W4 /WX: clang emits its own warning set, so a specific MSVC
# warning number (C4804, C4996) may differ. Treat this as the compile-and-ABI gate,
# and keep the real /WX warning verdict as the CI's job. It also does not run anything:
# Windows runtime behaviour (the named-pipe ACL semantics that the m3 assert was really
# about) still belongs to a real Windows kernel.
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

# Link the release dynamic CRT (msvcrt.lib) in every configuration. `xwin splat` ships
# only the release CRT by default, and the aqt Windows Qt kit is a release build linking
# the same, so pinning this keeps our targets matching Qt and avoids CMake's own
# compiler-detection test reaching for the absent debug msvcrtd.lib in a Debug probe.
# (Verified: without it the toolchain's compiler check fails on msvcrtd.lib; with it a
# QtCore-less PE links clean against this splat.)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# clang-cl defaults to the x86_64-pc-windows-msvc target, but pin it so the toolchain
# is explicit regardless of the driver's host default.
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# The LLVM Windows-flavoured binutils. lld-link is the linker; llvm-lib and llvm-rc
# stand in for lib.exe and rc.exe.
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_RC_COMPILER_INIT llvm-rc)

# The MSVC CRT and Windows SDK headers, handed to clang-cl as system includes (/imsvc)
# so their own warnings never trip a -Werror build of our code. Ordering mirrors what a
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
# Libraries, headers, and packages search normally (BOTH), not restricted to a sysroot.
# This toolchain has no single sysroot: the CRT and SDK arrive as raw linker flags above,
# the Windows Qt kit through CMAKE_PREFIX_PATH, and a Windows OpenSSL through
# OPENSSL_ROOT_DIR. FindOpenSSL uses find_library/find_path, so those must be allowed to
# resolve the paths handed to them. Picking up a host library by accident is not a risk:
# the target link suffix is .lib, so a Linux .so or .a can never match a find_library.
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
