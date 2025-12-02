# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0


# stall, built by SynQt.
#
# Everything `synqt build` generates lands in generated/, which this hands the
# build to. This file is yours: anything added below the include is kept.

cmake_minimum_required(VERSION 3.21)

# Here rather than in the generated file below it: CMake asks for a literal,
# direct project() in the top-level file and warns when it finds one only inside
# an include. It is also the one line here worth editing, if this build ever grows
# a language beyond C++.
project(stall LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/generated/synqt.cmake")
