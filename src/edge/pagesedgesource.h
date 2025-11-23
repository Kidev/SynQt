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

/// The web edge's own Source for the framework-supplied Pages connect point. WebEdge hosts
/// one per accepted connection, carrying that connection's own Caller. The PageStore and
/// PagesService behind it are shared across every connection, so this class holds no state
/// beyond the published routeTable.
///
/// fetchPage() decides nothing: it hands the request path, the caller-held hash and this
/// connection's Caller to PagesService::fetchPageFor() and returns its answer unchanged. The
/// confidentiality boundary lives in PagesService only.
class PagesEdgeSource : public PagesSimpleSource
{
    Q_OBJECT

public:
    /// store and service are shared across every connection and outlive this
    /// instance (owned by WebEdge). Caller is this connection's own and must never
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
