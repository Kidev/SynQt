# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.21)
project(Quick3DPhysWasm LANGUAGES CXX)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Quick Quick3D Quick3DPhysics)
qt_standard_project_setup(REQUIRES 6.11)

qt_add_executable(quick3dphys-wasm main.cpp)

qt_add_qml_module(quick3dphys-wasm
    URI Quick3DPhysWasm
    VERSION 1.0
    QML_FILES Main.qml
)

target_link_libraries(quick3dphys-wasm PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Quick3D
    Qt6::Quick3DPhysics
)
