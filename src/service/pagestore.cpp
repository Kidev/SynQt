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
#include <QTimer>

#include <utility>

namespace SynQt {

namespace {

/// How long the file has to stay quiet before it is read. Long enough that a truncate
/// and the write behind it coalesce into one reload, short enough that an edit still
/// reaches the browser as fast as the developer can look at it.
constexpr int reloadQuietMs{100};

/// How many reads a change is given before it is given up on. A file that was just
/// written can be briefly unreadable: an atomic replace leaves the path missing between
/// the unlink and the rename, and on Windows an indexer or a scanner holds a fresh file
/// open for a moment. Retrying for two seconds turns that into a slightly late hot
/// reload rather than an edit that never arrives.
constexpr int reloadAttempts{20};

} // namespace

PageStore::PageStore(QString pagesDir, QObject *parent)
    : QObject{parent}
    , m_pagesDir{std::move(pagesDir)}
{
}

PageStore::~PageStore() = default;

void PageStore::addPage(const QString &route, const QString &file, const QString &scope,
                        const QString &graphics)
{
    Page page{};
    page.file = file;
    page.scope = scope;
    page.graphics = graphics;
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
        // Only when it says something. A page with no requirement leaves the table byte
        // for byte what it was before this existed.
        if (!iterator.value().graphics.isEmpty()) {
            entry.insert(QStringLiteral("graphics"), iterator.value().graphics);
        }
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
        delete m_reloadTimer;
        m_reloadTimer = nullptr;
        m_pending.clear();
        return;
    }
    if (m_watcher) {
        return;
    }
    m_reloadTimer = new QTimer{this};
    m_reloadTimer->setSingleShot(true);
    connect(m_reloadTimer, &QTimer::timeout, this, &PageStore::flushPending);
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
    // Deliberately not read here. One edit is not one notification: an editor that
    // truncates and then writes produces two, and reading between them hashes an empty
    // file. Wait until the notifications stop, and let every further one push that wait
    // back.
    m_pending.insert(route, reloadAttempts);
    if (m_reloadTimer) {
        m_reloadTimer->start(reloadQuietMs);
    }
}

void PageStore::flushPending()
{
    // Over a copy: the loop writes to m_pending, and a route dropped here must not
    // invalidate the iteration.
    const QStringList routes{m_pending.keys()};
    for (const QString &route : routes) {
        const QString path{QDir{m_pagesDir}.filePath(m_pages.value(route).file)};
        const QString before{m_pages.value(route).hash};
        if (reload(route)) {
            m_pending.remove(route);
            // An atomic replace (write a sibling, rename over the watched path) drops the
            // watch with the old inode, so re-arm it on the file that is there now.
            if (m_watcher && !m_watcher->files().contains(path)) {
                m_watcher->addPath(path);
            }
            // Only when the content actually moved. A replace can be reported twice, and
            // an unchanged hash tells every open tab to re-fetch a page it already holds.
            if (m_pages.value(route).hash != before) {
                emit pageChanged(route, m_pages.value(route).hash);
            }
            continue;
        }
        const int attemptsLeft{m_pending.value(route) - 1};
        if (attemptsLeft > 0) {
            m_pending.insert(route, attemptsLeft);
            continue;
        }
        m_pending.remove(route);
        if (QFileInfo::exists(path)) {
            qWarning("SynQt: page file for route %s changed but could not be read; open "
                     "tabs keep the version they have",
                     qUtf8Printable(route));
        } else {
            qWarning("SynQt: page file for route %s is gone; its edits will "
                     "no longer reach open tabs until the dev server restarts",
                     qUtf8Printable(route));
        }
    }
    if (!m_pending.isEmpty() && m_reloadTimer) {
        m_reloadTimer->start(reloadQuietMs);
    }
}

} // namespace SynQt
