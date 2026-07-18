// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MONGOPROVIDER_H
#define SYNQT_MONGOPROVIDER_H

#include "idocumentprovider.h"
#include "providerconfig.h"

namespace SynQt {

/// An external document provider that wraps the MongoDB C driver (a thin, honest wrapper
/// over its client API, not a reimplementation), implementing the SAME IDocumentProvider
/// interface as the memory provider so a document entity's Source is unchanged when it moves
/// to MongoDB. Filters and documents cross as maps and are bridged to BSON here; the Source
/// never speaks the wire protocol. It connects over verified TLS and REFUSES a plaintext or
/// unverified connection in release. The connection string comes from the entity env only
/// (never on a connect point, never logged).
///
/// This class is compiled only when the MongoDB C driver is available (CMake sets
/// SYNQT_HAVE_MONGOC); the document factory returns it by name and falls back to a clear
/// error otherwise. The client handle is held as void* so this header pulls in no mongoc.
class MongoDocumentProvider final : public IDocumentProvider
{
public:
    explicit MongoDocumentProvider(ProviderConfig config);
    ~MongoDocumentProvider() override;

    bool connect(QString *error) override;
    void disconnect() override;
    bool isHealthy() const override;

    QVariant insert(const QString &collection, const QVariantMap &document) override;
    QVariantList find(const QString &collection, const QVariantMap &filter,
                      const QVariantMap &options) override;
    int update(const QString &collection, const QVariantMap &filter,
               const QVariantMap &change) override;
    int remove(const QString &collection, const QVariantMap &filter) override;

    QString name() const override;

    /// The insecure-connection guard, exposed for testing: true when this config must be
    /// refused (release without TLS on the connection).
    bool refusesInsecure() const;

private:
    ProviderConfig m_config;
    void *m_client{nullptr};   // mongoc_client_t*
};

} // namespace SynQt

#endif // SYNQT_MONGOPROVIDER_H
