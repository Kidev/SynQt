// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "pagestore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace SynQt {

PageStore::PageStore(QString pagesDir, QObject *parent)
    : QObject{parent}
    , m_pagesDir{std::move(pagesDir)}
{
}

PageStore::~PageStore() = default;

void PageStore::addPage(const QString &route, const QString &file, const QString &scope)
{
    Page page{};
    page.file = file;
    page.scope = scope;
    m_pages.insert(route, page);
    m_routeByFile.insert(QDir{m_pagesDir}.filePath(file), route);
    if (!reload(route)) {
        qWarning("SynQt: page file for route %s is unreadable", qUtf8Printable(route));
    }
    if (m_watcher) {
        m_watcher->addPath(QDir{m_pagesDir}.filePath(file));
    }
}

bool PageStore::reload(const QString &route)
{
    const auto iterator{m_pages.find(route)};
    if (iterator == m_pages.end()) {
        return false;
    }
    QFile file{QDir{m_pagesDir}.filePath(iterator->file)};
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes{file.readAll()};
    file.close();
    iterator->source = QString::fromUtf8(bytes);
    iterator->hash = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return true;
}

bool PageStore::hasRoute(const QString &route) const
{
    return m_pages.contains(route);
}

QString PageStore::scopeFor(const QString &route) const
{
    return m_pages.value(route).scope;
}

QString PageStore::hashFor(const QString &route) const
{
    return m_pages.value(route).hash;
}

QString PageStore::sourceFor(const QString &route) const
{
    return m_pages.value(route).source;
}

QString PageStore::routeTableJson() const
{
    QJsonArray table{};
    for (auto iterator{m_pages.constBegin()}; iterator != m_pages.constEnd(); ++iterator) {
        QJsonObject entry{};
        entry.insert(QStringLiteral("path"), iterator.key());
        entry.insert(QStringLiteral("scope"), iterator.value().scope);
        table.append(entry);
    }
    return QString::fromUtf8(QJsonDocument{table}.toJson(QJsonDocument::Compact));
}

QStringList PageStore::declaredRoutes() const
{
    return m_pages.keys();
}

void PageStore::setWatching(bool watching)
{
    if (!watching) {
        delete m_watcher;
        m_watcher = nullptr;
        return;
    }
    if (m_watcher) {
        return;
    }
    m_watcher = new QFileSystemWatcher{this};
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &PageStore::onFileChanged);
    for (auto iterator{m_pages.constBegin()}; iterator != m_pages.constEnd(); ++iterator) {
        m_watcher->addPath(QDir{m_pagesDir}.filePath(iterator.value().file));
    }
}

void PageStore::onFileChanged(const QString &path)
{
    const QString route{m_routeByFile.value(path)};
    if (route.isEmpty()) {
        return;
    }
    const bool reloaded{reload(route)};
    // An atomic replace (write a sibling, rename over the watched path) can
    // deliver this notification while the old inode is briefly gone, so
    // reload() above may have failed; recovery below must run regardless.
    if (m_watcher && !m_watcher->files().contains(path)) {
        if (QFileInfo::exists(path)) {
            m_watcher->addPath(path);
        } else {
            qWarning("SynQt: page file for route %s is gone; its edits will "
                     "no longer reach open tabs until the dev server restarts",
                     qUtf8Printable(route));
        }
    }
    if (reloaded) {
        emit pageChanged(route, m_pages.value(route).hash);
    }
}

} // namespace SynQt
