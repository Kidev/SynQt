// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "tracescope.h"

namespace SynQt {

namespace {

TraceContext &installed()
{
    // Per thread for the same reason ActingFor's caller is. One entity is one event loop
    // today, and "which trace is this work part of" must not become a fact about how the
    // runtime happens to be scheduled. A plain value, so a thread ending destroys a
    // QString and nothing else.
    static thread_local TraceContext context;
    return context;
}

} // namespace

TraceScope::TraceScope(const TraceContext &context)
    : m_displaced{installed()}
{
    installed() = context;
}

TraceScope::~TraceScope()
{
    installed() = m_displaced;
}

TraceContext TraceScope::current()
{
    return installed();
}

} // namespace SynQt
