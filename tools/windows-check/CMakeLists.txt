# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The QtCore probe target for the Windows cross-compile gate. Built through
# cmake/toolchains/windows-clang-cl.cmake by tools/windows-check/check-windows.sh; see
# that script and README.md. Kept trivial so the first thing the gate proves is the
# toolchain itself, not any framework code.

cmake_minimum_required(VERSION 3.21)
project(synqt_windows_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core)
qt_standard_project_setup()

# Console subsystem (no WIN32), so the probe's stdout line is visible when it is run on
# a real Windows host or under an emulator.
qt_add_executable(synqt-win-probe probe.cpp)
target_link_libraries(synqt-win-probe PRIVATE Qt6::Core)
