# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The SynQt test harness: the `SynQt.Test` QML import an application's own tests use to
# drive one owned connect point Source with a caller they choose.
#
# It is a separate library on purpose. A production entity links SynQtService and never
# this, and writes `import SynQt` and never `import SynQt.Test`, so nothing here can widen
# the runtime's surface: the harness reaches Caller through the same public factory an
# entity's transport does, and there is no test-only door in Caller for it to use.

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Qml GLOBAL)

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
