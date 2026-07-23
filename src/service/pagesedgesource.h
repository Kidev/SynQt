// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PAGESEDGESOURCE_H
#define SYNQT_PAGESEDGESOURCE_H

#include "rep_pages_source.h"

#include <QObject>
#include <QString>

namespace SynQt {

class Caller;
class PageStore;
class PagesService;

/// The web edge's own Source for the framework-supplied Pages connect point (see
/// docs on remote pages, Plan B). WebEdge hosts one instance of this per accepted
/// connection, exactly the way it hosts a per_session application connect point: a
/// fresh Source carrying that connection's own Caller. The PageStore and
/// PagesService it is built over are shared across every connection (the page
/// table is the same for everyone), so this class holds no state of its own beyond
/// what the generated base already tracks (the published routeTable).
///
/// fetchPage() does not decide anything: it hands the concrete request path, the
/// caller-held hash, and this connection's own Caller to
/// PagesService::fetchPageFor() and returns its answer unchanged. The
/// confidentiality boundary lives in PagesService, once, and only there.
class PagesEdgeSource : public PagesSimpleSource
{
    Q_OBJECT

public:
    /// store and service are shared across every connection and outlive this
    /// instance (owned by WebEdge); caller is this connection's own and must never
    /// be shared with another connection's Source.
    PagesEdgeSource(PageStore *store, PagesService *service, Caller *caller,
                    QObject *parent = nullptr);

    PageResponse fetchPage(QString route, QString haveHash) override;

private:
    void onPageChanged(const QString &route, const QString &hash);

    PageStore *m_store;
    PagesService *m_service;
    Caller *m_caller;
};

} // namespace SynQt

#endif // SYNQT_PAGESEDGESOURCE_H
