// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PAGESSERVICE_H
#define SYNQT_PAGESSERVICE_H

#include "rep_pages_source.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

namespace SynQt {

class Caller;
class PageStore;

/// Owner-side answer to "may this caller have this page, and do they already have it".
///
/// This is where a remote page's confidentiality actually lives. The client-side route
/// guard is a redirect and protects nothing; the check here is what stops an under-scoped
/// session from receiving a single byte of the page. Requests name a route, and a route
/// is matched against the declared table, so no caller-supplied string ever reaches the
/// filesystem.
class PagesService : public QObject
{
    Q_OBJECT

public:
    /// Builds the seed a page paints with before its connect points push anything. It
    /// runs after the scope check, so it may read privileged state, and it receives the
    /// caller so it can scope what it returns.
    using SeedProvider =
        std::function<QString(const QString &route, const QVariantMap &parameters,
                              Caller *caller)>;

    /// store must not be null: it is the page table this service answers every
    /// fetchPageFor() call against, not an optional collaborator, so a null
    /// store is a construction-time programming error, not a per-call check.
    explicit PagesService(PageStore *store, QObject *parent = nullptr);
    ~PagesService() override;

    void setSeedProvider(SeedProvider provider);

    /// requestPath is the concrete path asked for ("/c/summer"), matched against the
    /// declared route patterns. haveHash is the content hash the caller already holds.
    PageResponse fetchPageFor(const QString &requestPath, const QString &haveHash,
                              Caller *caller);

private:
    PageStore *m_store;
    SeedProvider m_seedProvider;
};

} // namespace SynQt

#endif // SYNQT_PAGESSERVICE_H
