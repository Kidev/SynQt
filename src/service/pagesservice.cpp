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

/// A declared route paired with its compiled pattern, so match precedence is
/// decided once (by literalSegmentCount(), most literal first) rather than left
/// to PageStore::declaredRoutes()'s QHash order, which is unspecified and
/// unstable. Mirrors Router::compiledRoutes()/applyRoutes() on the client side.
struct Candidate
{
    QString route;
    RoutePattern pattern;
};

QList<Candidate> orderedCandidates(const QStringList &declared)
{
    QList<Candidate> candidates{};
    for (const QString &route : declared) {
        RoutePattern pattern{route};
        if (!pattern.isValid()) {
            qWarning("SynQt: ignoring malformed declared route pattern %s",
                     qUtf8Printable(route));
            continue;
        }
        candidates.append(Candidate{route, std::move(pattern)});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) {
        return a.pattern.literalSegmentCount() > b.pattern.literalSegmentCount();
    });
    return candidates;
}

} // namespace

PagesService::PagesService(PageStore *store, QObject *parent)
    : QObject{parent}
    , m_store{store}
{
}

PagesService::~PagesService() = default;

void PagesService::setSeedProvider(SeedProvider provider)
{
    m_seedProvider = std::move(provider);
}

PageResponse PagesService::fetchPageFor(const QString &requestPath,
                                        const QString &haveHash, Caller *caller)
{
    QVariantMap query{};
    const QString path{RoutePattern::splitQuery(requestPath, &query)};

    // Match against what was declared, most literal segments first (see
    // orderedCandidates() above), so "/c/summary" beats "/c/:campaign" whatever
    // order PageStore::declaredRoutes() happened to return them in. A route the
    // table does not contain does not exist, whatever the caller sent.
    QString matched{};
    QVariantMap parameters{};
    const QList<Candidate> candidates{orderedCandidates(m_store->declaredRoutes())};
    for (const Candidate &candidate : candidates) {
        QVariantMap captured{};
        if (candidate.pattern.matches(path, &captured)) {
            matched = candidate.route;
            parameters = captured;
            break;
        }
    }
    if (matched.isEmpty()) {
        return refusal(QStringLiteral("notFound"));
    }

    const QString scope{m_store->scopeFor(matched)};
    if (!scope.isEmpty() && (!caller || !caller->hasScope(scope))) {
        // Nothing about the page goes back: not its source, not its hash, not
        // its size.
        return refusal(QStringLiteral("forbidden"));
    }

    // Past this point the caller is authorized for this route, so a reply may carry the
    // page. The hash is the hash of the page FILE, so it is the same for every
    // parameterization of one route: a caller who already holds the component sends that
    // hash with a different concrete path, and only the bulky qml payload is worth
    // skipping. The seed is small and parameter-dependent, and the client keeps its
    // previous seed on an empty one (router.cpp), so producing it only on the ok path
    // would paint the new parameters with the old page's data.
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
