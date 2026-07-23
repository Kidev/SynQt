// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_REMOTEPAGELOADER_H
#define SYNQT_REMOTEPAGELOADER_H

#include "qmlpalette.h"

#include <QHash>
#include <QObject>
#include <QPointer>
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

    /// Forget every route, so the next resolution to any of them refetches. Called on a
    /// scope change: a page (and the seed it was delivered with) fetched under one scope
    /// must not go on being shown, unconfirmed, to a session that no longer holds it.
    void clear();

private:
    struct CachedPage
    {
        QString hash;
        // QPointer, not a raw pointer: this is the sole owner of the component (deleted
        // in deliver()/invalidate()/clear()/the destructor), but a defensive belt in
        // case anything else ever frees it out from under this cache too. A cleared
        // QPointer reads as a cache miss everywhere a CachedPage's component is used,
        // never as a stale, already-freed pointer handed back out.
        QPointer<QQmlComponent> component{};
    };

    QQmlEngine *m_engine;
    QmlPalette m_palette;
    QHash<QString, CachedPage> m_pages;
};

} // namespace SynQt

#endif // SYNQT_REMOTEPAGELOADER_H
