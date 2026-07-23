// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_REMOTEPAGELOADER_H
#define SYNQT_REMOTEPAGELOADER_H

#include "qmlpalette.h"

#include <QHash>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QQmlComponent;
class QQmlEngine;
QT_END_NAMESPACE

namespace SynQt {

/// Turns a page the edge delivered into something the app can render.
///
/// Delivered QML is interpreted: qmlcachegen and the ahead-of-time compiler only apply to
/// QML compiled into the bundle, and WebAssembly has no JIT. That is the right trade for a
/// form, a report, or a campaign page, and the wrong one for anything running per frame.
///
/// Components are cached by content hash, so a revisit costs nothing and an unchanged page
/// is never re-parsed. The cache is memory only: a reload refetches, which keeps a stale
/// page from outliving a deploy.
class RemotePageLoader : public QObject
{
    Q_OBJECT

public:
    enum class Outcome
    {
        Ready,       ///< a component was built and is available from componentFor()
        NotModified, ///< the cached component for this hash is still current
        Rejected,    ///< the palette refused the page; nothing was instantiated
        Failed,      ///< the page did not parse
    };
    Q_ENUM(Outcome)

    RemotePageLoader(QQmlEngine *engine, QmlPalette palette, QObject *parent = nullptr);
    ~RemotePageLoader() override;

    /// Accept a delivered page. Pass an empty source with a hash already held to confirm
    /// the cache. reason, when given, receives why a page was rejected or failed.
    Outcome deliver(const QString &route, const QString &source, const QString &hash,
                    QString *reason);

    QQmlComponent *componentFor(const QString &route) const;
    QString hashFor(const QString &route) const;

    /// Forget a route, so the next visit refetches. Called when the edge reports the page
    /// changed.
    void invalidate(const QString &route);

private:
    struct CachedPage
    {
        QString hash;
        QQmlComponent *component{nullptr};
    };

    QQmlEngine *m_engine;
    QmlPalette m_palette;
    QHash<QString, CachedPage> m_pages;
};

} // namespace SynQt

#endif // SYNQT_REMOTEPAGELOADER_H
