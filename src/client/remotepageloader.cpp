// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "remotepageloader.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>

#include <utility>

namespace SynQt {

RemotePageLoader::RemotePageLoader(QQmlEngine *engine, QmlPalette palette, QObject *parent)
    : QObject{parent}
    , m_engine{engine}
    , m_palette{std::move(palette)}
{
}

RemotePageLoader::~RemotePageLoader() = default;

RemotePageLoader::Outcome RemotePageLoader::deliver(const QString &route,
                                                    const QString &source,
                                                    const QString &hash, QString *reason)
{
    const auto cached{m_pages.constFind(route)};
    // cached->component is a QPointer: a null value here means the cache miss it looks
    // like, even if something outside this class already freed what used to be there,
    // rather than confirming a component that no longer exists as still current.
    if (cached != m_pages.constEnd() && cached->hash == hash && cached->component) {
        return Outcome::NotModified;
    }

    QString why;
    if (!m_palette.isAcceptable(source, &why)) {
        if (reason) {
            *reason = why;
        }
        return Outcome::Rejected;
    }

    // A stable, route-derived URL so error messages name the page and relative resolution
    // has something to resolve against.
    const QUrl url{QStringLiteral("synqt://page%1").arg(route)};
    auto *component{new QQmlComponent{m_engine, this}};
    component->setData(source.toUtf8(), url);
    if (component->isError()) {
        if (reason) {
            *reason = component->errorString();
        }
        component->deleteLater();
        return Outcome::Failed;
    }

    QQmlComponent *previous{cached != m_pages.constEnd() ? cached->component : nullptr};
    m_pages.insert(route, CachedPage{hash, component});
    if (previous) {
        previous->deleteLater();
    }
    return Outcome::Ready;
}

QQmlComponent *RemotePageLoader::componentFor(const QString &route) const
{
    return m_pages.value(route).component;
}

QString RemotePageLoader::hashFor(const QString &route) const
{
    return m_pages.value(route).hash;
}

void RemotePageLoader::invalidate(const QString &route)
{
    const auto cached{m_pages.constFind(route)};
    if (cached == m_pages.constEnd()) {
        return;
    }
    if (cached->component) {
        cached->component->deleteLater();
    }
    m_pages.remove(route);
}

void RemotePageLoader::clear()
{
    for (const CachedPage &page : std::as_const(m_pages)) {
        if (page.component) {
            page.component->deleteLater();
        }
    }
    m_pages.clear();
}

} // namespace SynQt
