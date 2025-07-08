// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IDOCUMENTPROVIDER_H
#define SYNQT_IDOCUMENTPROVIDER_H

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace SynQt {

/// The document family interface. Every provider (mongodb, ...) implements exactly this;
/// the connect point Source calls it and never speaks the engine's wire protocol. Filters
/// and documents are passed as maps, never as an engine-specific query string.
class IDocumentProvider
{
public:
    virtual ~IDocumentProvider() = default;

    virtual bool connect(QString *error) = 0;
    virtual void disconnect() = 0;
    virtual bool isHealthy() const = 0;

    virtual QVariant insert(const QString &collection, const QVariantMap &document) = 0; ///< id
    virtual QVariantList find(const QString &collection, const QVariantMap &filter,
                              const QVariantMap &options) = 0;
    virtual int update(const QString &collection, const QVariantMap &filter,
                       const QVariantMap &change) = 0;
    virtual int remove(const QString &collection, const QVariantMap &filter) = 0;

    virtual QString name() const = 0;
};

} // namespace SynQt

#endif // SYNQT_IDOCUMENTPROVIDER_H
