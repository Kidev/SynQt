// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "pagesedgesource.h"

#include "caller.h"
#include "pagesservice.h"
#include "pagestore.h"

namespace SynQt {

PagesEdgeSource::PagesEdgeSource(PageStore *store, PagesService *service, Caller *caller,
                                 QObject *parent)
    : PagesSimpleSource{parent}
    , m_store{store}
    , m_service{service}
    , m_caller{caller}
{
    setRouteTable(m_store->routeTableJson());
    connect(m_store, &PageStore::pageChanged, this, &PagesEdgeSource::onPageChanged);
}

PageResponse PagesEdgeSource::fetchPage(QString route, QString haveHash)
{
    // The one and only authorization decision for a page fetch. m_caller is this
    // connection's own (never another connection's), and PagesService is the
    // confidentiality boundary: its answer is returned exactly as given, with no
    // scope check duplicated here.
    return m_service->fetchPageFor(route, haveHash, m_caller);
}

void PagesEdgeSource::onPageChanged(const QString &route, const QString &hash)
{
    // The declared paths and scopes cannot actually change after WebEdge::start()
    // (addPage runs once, at startup), but re-publishing here is what keeps this
    // per-connection copy honest if that ever stops being true.
    setRouteTable(m_store->routeTableJson());
    emit pageChanged(route, hash);
}

} // namespace SynQt
