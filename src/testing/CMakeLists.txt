# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt test harness: the `SynQt.Test` QML import an application's own tests use to
# drive one owned connect point Source with a caller they choose.
#
# Never linked by a production entity. It uses Caller's public factories only.

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Qml GLOBAL)

qt_add_library(SynQtTesting STATIC
    entitytest.cpp
    entitytest.h
)

set_target_properties(SynQtTesting PROPERTIES AUTOMOC ON)

target_include_directories(SynQtTesting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# SynQtService for Caller and SessionManager, SynQtProviders for the helpers the Source
# calls. Public because a test main constructs the harness and needs both.
target_link_libraries(SynQtTesting PUBLIC
    Qt6::Core
    Qt6::Qml
    SynQtService
    SynQtProviders
)
