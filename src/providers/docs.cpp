// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "docs.h"

#include "idocumentprovider.h"

namespace SynQt {

Docs::Docs(IDocumentProvider *provider, QObject *parent)
    : QObject{parent}
    , m_provider{provider}
{
}

QVariant Docs::insert(const QString &collection, const QVariantMap &document)
{
    return m_provider->insert(collection, document);
}

QVariantList Docs::find(const QString &collection, const QVariantMap &filter,
                        const QVariantMap &options)
{
    return m_provider->find(collection, filter, options);
}

int Docs::update(const QString &collection, const QVariantMap &filter, const QVariantMap &change)
{
    return m_provider->update(collection, filter, change);
}

int Docs::remove(const QString &collection, const QVariantMap &filter)
{
    return m_provider->remove(collection, filter);
}

} // namespace SynQt
