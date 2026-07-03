// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "tracescope.h"

namespace SynQt {

namespace {

TraceContext &installed()
{
    // Per thread, like ActingFor's caller, so the current trace does not depend on how the
    // runtime is scheduled. A plain value, so thread exit only destroys a QString.
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

bool TraceScope::stampCurrent(QString &traceId, QString &spanId)
{
    const TraceContext &context{installed()};
    if (!context.isValid()) {
        return false;
    }
    traceId = context.traceId;
    spanId = context.spanId;
    return true;
}

} // namespace SynQt
