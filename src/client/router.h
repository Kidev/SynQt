// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ROUTER_H
#define SYNQT_ROUTER_H

#include "synclientconfig.h"

#include <QObject>
#include <QString>

namespace SynQt {

class Session;

/// Scope-gated navigation over the route table (docs/runtime-api.md). A route guard is
/// a redirect rule, NOT a secrecy mechanism: every view's QML ships to every visitor,
/// so a guard only steers navigation, while the data behind a privileged view still
/// arrives only through scope-gated connect points the edge refuses to an under-scoped
/// session.
class Router : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)

public:
    Router(SynClientConfig config, Session *session, QObject *parent = nullptr);

    QString path() const;

    /// Navigate to path; if the matched route declares a scope the session lacks,
    /// redirect to router.fallback instead.
    Q_INVOKABLE void go(const QString &path);

signals:
    void pathChanged();

private:
    SynClientConfig m_config;
    Session *m_session;
    QString m_path;
};

} // namespace SynQt

#endif // SYNQT_ROUTER_H
