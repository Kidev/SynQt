// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "pagesservice.h"

#include "caller.h"
#include "pagestore.h"
#include "routepattern.h"

#include <QList>

#include <algorithm>
#include <utility>

namespace SynQt {

namespace {

PageResponse refusal(const QString &status)
{
    PageResponse response{};
    response.setStatus(status);
    return response;
}

} // namespace

/// A declared route with its compiled pattern, so match precedence is decided once
/// (literalSegmentCount(), most literal first) instead of by PageStore::declaredRoutes()'s
/// unspecified QHash order. Mirrors Router::compiledRoutes()/applyRoutes() on the client.
struct PagesService::Candidate
{
    QString route;
    RoutePattern pattern;
};

PagesService::PagesService(PageStore *store, QObject *parent)
    : QObject{parent}
    , m_store{store}
{
}

PagesService::~PagesService() = default;

const QList<PagesService::Candidate> &PagesService::candidates() const
{
    // Keyed on the count: only PageStore::addPage() writes the table, and a hot reload
    // changes a page's bytes, not its route, so an unchanged size means an unchanged table.
    if (m_compiledRoutes == m_store->routeCount()) {
        return m_candidates;
    }
    const QStringList declared{m_store->declaredRoutes()};
    m_candidates.clear();
    m_candidates.reserve(declared.size());
    for (const QString &route : declared) {
        RoutePattern pattern{route};
        if (!pattern.isValid()) {
            qWarning("SynQt: ignoring malformed declared route pattern %s",
                     qUtf8Printable(route));
            continue;
        }
        m_candidates.append(Candidate{route, std::move(pattern)});
    }
    std::stable_sort(m_candidates.begin(), m_candidates.end(),
                     [](const Candidate &a, const Candidate &b) {
        return a.pattern.literalSegmentCount() > b.pattern.literalSegmentCount();
    });
    m_compiledRoutes = declared.size();
    return m_candidates;
}

void PagesService::setSeedProvider(SeedProvider provider)
{
    m_seedProvider = std::move(provider);
}

PageResponse PagesService::fetchPageFor(const QString &requestPath,
                                        const QString &haveHash, Caller *caller)
{
    QVariantMap query{};
    const QString path{RoutePattern::splitQuery(requestPath, &query)};

    // Match against the declared routes, most literal first (see candidates() above), so
    // "/c/summary" beats "/c/:campaign". An undeclared route does not exist.
    QString matched{};
    QVariantMap parameters{};
    QStringList segments;
    // One split for the table. See Api::dispatch. This runs on every page fetch.
    if (RoutePattern::splitPath(path, &segments)) {
        for (const Candidate &candidate : candidates()) {
            QVariantMap captured{};
            if (candidate.pattern.matches(segments, &captured)) {
                matched = candidate.route;
                parameters = captured;
                break;
            }
        }
    }
    if (matched.isEmpty()) {
        return refusal(QStringLiteral("notFound"));
    }

    const QString scope{m_store->scopeFor(matched)};
    if (!scope.isEmpty() && (!caller || !caller->hasScope(scope))) {
        // Nothing about the page goes back: no source, hash or size.
        return refusal(QStringLiteral("forbidden"));
    }

    // The caller is authorized for this route from here on. The hash is of the page file,
    // the same for every parameterization, so a caller holding the component may send it
    // with a different path and only the QML payload is skipped. The seed is small and
    // depends on the parameters, and the client keeps its previous seed on an empty one
    // (router.cpp), so it is produced on every reply.
    const QString hash{m_store->hashFor(matched)};
    const bool alreadyHeld{!haveHash.isEmpty() && haveHash == hash};

    PageResponse response{};
    response.setStatus(alreadyHeld ? QStringLiteral("notModified") : QStringLiteral("ok"));
    if (!alreadyHeld) {
        response.setQml(m_store->sourceFor(matched));
    }
    response.setHash(hash);
    if (m_seedProvider) {
        response.setSeed(m_seedProvider(matched, parameters, caller));
    }
    return response;
}

} // namespace SynQt
