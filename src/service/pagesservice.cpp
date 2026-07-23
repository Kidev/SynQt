// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "pagesservice.h"

#include "caller.h"
#include "pagestore.h"
#include "routepattern.h"

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

    // Match against what was declared. A route the table does not contain does not exist,
    // whatever the caller sent.
    QString matched{};
    QVariantMap parameters{};
    const QStringList declared{m_store->declaredRoutes()};
    for (const QString &candidate : declared) {
        QVariantMap captured{};
        const RoutePattern pattern{candidate};
        if (pattern.matches(path, &captured)) {
            matched = candidate;
            parameters = captured;
            break;
        }
    }
    if (matched.isEmpty()) {
        return refusal(QStringLiteral("notFound"));
    }

    const QString scope{m_store->scopeFor(matched)};
    if (!scope.isEmpty() && (!caller || !caller->hasScope(scope))) {
        // Nothing about the page goes back: not its source, not its hash, not its size.
        return refusal(QStringLiteral("forbidden"));
    }

    const QString hash{m_store->hashFor(matched)};
    if (!haveHash.isEmpty() && haveHash == hash) {
        PageResponse response{refusal(QStringLiteral("notModified"))};
        response.setHash(hash);
        return response;
    }

    PageResponse response{};
    response.setStatus(QStringLiteral("ok"));
    response.setQml(m_store->sourceFor(matched));
    response.setHash(hash);
    if (m_seedProvider) {
        response.setSeed(m_seedProvider(matched, parameters, caller));
    }
    return response;
}

} // namespace SynQt
