// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PAGESSERVICE_H
#define SYNQT_PAGESSERVICE_H

#include "rep_pages_source.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

namespace SynQt {

class Caller;
class PageStore;

/// Owner-side answer to "may this caller have this page, and do they already have it". This
/// check, not the client's route guard, is what keeps a page from an under-scoped session.
/// Requests name a route matched against the declared table, so no caller string reaches the
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

    /// store must not be null. It is the page table this service answers every
    /// fetchPageFor() call against, so a null store is a programming error
    /// caught at construction.
    explicit PagesService(PageStore *store, QObject *parent = nullptr);
    ~PagesService() override;

    void setSeedProvider(SeedProvider provider);

    /// requestPath is the concrete path asked for ("/c/summer"), matched against the
    /// declared route patterns. haveHash is the content hash the caller already holds.
    PageResponse fetchPageFor(const QString &requestPath, const QString &haveHash,
                              Caller *caller);

private:
    /// A declared route paired with its compiled pattern, in match order.
    struct Candidate;

    /// The route table, compiled once and kept in matching order. Built lazily, since pages are
    /// added after this service exists, and rebuilt when the declared set changes.
    const QList<Candidate> &candidates() const;

    PageStore *m_store;
    SeedProvider m_seedProvider;
    mutable QList<Candidate> m_candidates;
    mutable qsizetype m_compiledRoutes{-1};
};

} // namespace SynQt

#endif // SYNQT_PAGESSERVICE_H
