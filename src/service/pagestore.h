// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PAGESTORE_H
#define SYNQT_PAGESTORE_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QFileSystemWatcher;
QT_END_NAMESPACE

namespace SynQt {

/// The edge's page files, their content hashes, and the route table it
/// publishes.
///
/// This is the only component that touches page files. A route is looked up
/// by exact match against what was declared, never by joining a
/// caller-supplied string onto the pages directory, so there is no path a
/// request can name that was not configured.
///
/// Hashing is what makes a revisit free: the client sends the hash it holds
/// and an unchanged page comes back as a status with no body. In development
/// the store watches the directory so an edit reaches open tabs; in release
/// it reads and hashes once.
class PageStore : public QObject
{
    Q_OBJECT

public:
    explicit PageStore(QString pagesDir, QObject *parent = nullptr);
    ~PageStore() override;

    /// Declare a page. file is relative to the pages directory; scope is the
    /// minimum session scope, empty for a page any session may fetch.
    void addPage(const QString &route, const QString &file, const QString &scope);

    bool hasRoute(const QString &route) const;
    QString scopeFor(const QString &route) const;
    QString hashFor(const QString &route) const;
    QString sourceFor(const QString &route) const;

    /// The remote route table as a JSON array of {path, scope} objects, for
    /// the Pages connect point to push.
    QString routeTableJson() const;

    /// Every declared route, for matching a request path against the table.
    QStringList declaredRoutes() const;

    /// Watch the page files and re-hash on change. Development only.
    void setWatching(bool watching);

signals:
    void pageChanged(const QString &route, const QString &hash);

private:
    struct Page
    {
        QString file;
        QString scope;
        QString hash;
        QString source;
    };

    bool reload(const QString &route);
    void onFileChanged(const QString &path);

    QString m_pagesDir;
    QHash<QString, Page> m_pages;
    QHash<QString, QString> m_routeByFile;
    QFileSystemWatcher *m_watcher{nullptr};
};

} // namespace SynQt

#endif // SYNQT_PAGESTORE_H
