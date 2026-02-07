// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "log.h"

#include "tracer.h"

namespace SynQt {

Log::Log(QObject *parent)
    : QObject{parent}
{
}

void Log::debug(const QString &message, const QVariantMap &attributes)
{
    trace(Category::Application, Severity::Debug, message, attributes);
}

void Log::info(const QString &message, const QVariantMap &attributes)
{
    trace(Category::Application, Severity::Info, message, attributes);
}

void Log::warn(const QString &message, const QVariantMap &attributes)
{
    trace(Category::Application, Severity::Warning, message, attributes);
}

void Log::error(const QString &message, const QVariantMap &attributes)
{
    trace(Category::Application, Severity::Error, message, attributes);
}

} // namespace SynQt
