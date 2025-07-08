// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "db.h"

#include "ipersistenceprovider.h"

#include <QVariantMap>

namespace SynQt {

Db::Db(IPersistenceProvider *provider, QObject *parent)
    : QObject{parent}
    , m_provider{provider}
{
}

QVariantList Db::query(const QString &sql, const QVariantList &params)
{
    const DbResult result{m_provider->query(sql, params)};
    if (!result.ok) {
        m_lastError = result.error;
        emit errorOccurred(result.error);
        return {};
    }
    return result.rows;
}

QVariantMap Db::exec(const QString &sql, const QVariantList &params)
{
    const DbResult result{m_provider->exec(sql, params)};
    if (!result.ok) {
        m_lastError = result.error;
        emit errorOccurred(result.error);
        return {};
    }
    return QVariantMap{{QStringLiteral("affected"), result.affected},
                       {QStringLiteral("insertId"), result.insertId}};
}

QString Db::lastError() const
{
    return m_lastError;
}

} // namespace SynQt
