// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_DOCS_H
#define SYNQT_DOCS_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace SynQt {

class IDocumentProvider;

/// The document helper exposed to a document entity's QML as `Docs`. It forwards to
/// whichever IDocumentProvider backs the entity, so a Source never speaks an engine's wire
/// protocol: filters and documents are passed as maps, never as an engine query string,
/// which is what keeps the same Source working when the provider changes from `memory` to
/// `mongodb`. Credentials stay inside the provider.
class Docs : public QObject
{
    Q_OBJECT

public:
    explicit Docs(IDocumentProvider *provider, QObject *parent = nullptr);

    /// Insert one document; returns its id, or an invalid QVariant on failure.
    Q_INVOKABLE QVariant insert(const QString &collection, const QVariantMap &document);
    /// Matching documents, newest storage order; empty when nothing matches or on failure.
    Q_INVOKABLE QVariantList find(const QString &collection,
                                  const QVariantMap &filter = QVariantMap(),
                                  const QVariantMap &options = QVariantMap());
    /// Apply `change` to every document matching `filter`; returns how many changed.
    Q_INVOKABLE int update(const QString &collection, const QVariantMap &filter,
                           const QVariantMap &change);
    /// Remove every document matching `filter`; returns how many were removed.
    Q_INVOKABLE int remove(const QString &collection, const QVariantMap &filter);

private:
    IDocumentProvider *m_provider;
};

} // namespace SynQt

#endif // SYNQT_DOCS_H
