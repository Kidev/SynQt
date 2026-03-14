// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SESSION_H
#define SYNQT_SESSION_H

#include "synclientconfig.h"

#include <QJSValue>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

QT_BEGIN_NAMESPACE
class QJSEngine;
QT_END_NAMESPACE

namespace SynQt {

class ScopeCheck;

/// Read-only session state plus the two actions that change it (see the
/// [runtime API reference](https://synqt.org/runtime-api/)).
/// It never exposes a secret: the raw session id and any token live at the edge, not in
/// the client. The framework, not app code, drives state/scope/identity. Login/logout
/// are surfaced to QML.
///
/// \sa \ref qmlsession "the Session accessor page"
class Session : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QVariant scope READ scope NOTIFY scopeChanged)
    Q_PROPERTY(QVariant identity READ identity NOTIFY identityChanged)
    Q_PROPERTY(bool isAuthenticated READ isAuthenticated NOTIFY identityChanged)
    /// The scope check, and a property rather than a Q_INVOKABLE.
    ///
    /// QML records a binding's dependencies from the properties it reads, and from
    /// nothing else. A method call is invisible to it. Written as an invokable,
    /// `visible: !Session.hasScope("player")` is evaluated once, while the visitor is
    /// still anonymous, and never again, so signing in never lifts the gate it was
    /// written to lift. Reading it as a property registers `scopeChanged`. The value
    /// read is the check itself, so `Session.hasScope("player")` still spells a call
    /// and now re-runs whenever the scope moves.
    Q_PROPERTY(QJSValue hasScope READ scopeCheck NOTIFY scopeChanged)

public:
    /// \a engine is the app's own QML engine, and the only thing it is used for is
    /// building the `hasScope` function above. A Session built without one still
    /// answers every C++ caller. Only the QML-side check needs an engine to exist in.
    explicit Session(SynClientConfig config, QJSEngine *engine = nullptr,
                     QObject *parent = nullptr);

    QString state() const;
    QVariant scope() const;
    QVariant identity() const;
    bool isAuthenticated() const;
    QJSValue scopeCheck() const;

    bool hasScope(const QString &name) const;

    Q_INVOKABLE void login(const QString &provider = QString());
    Q_INVOKABLE void logout();

    /// Framework-side setters (driven by SynClient / the edge), not app-facing.
    void setState(const QString &state);
    void setScope(const QVariant &scope);
    void setIdentity(const QVariant &identity);

    /// Both at once, which is how the edge says them.
    ///
    /// Scope and identity change together and QML reads them together: an app asks
    /// `hasScope` and then names the visitor, usually in one expression. Setting them one
    /// after the other gives every such binding an evaluation in between where the scope
    /// has moved and the identity has not, so a sign-in paints once as "elevated and
    /// nobody" before it paints correctly. Both members are written before either signal
    /// goes out, so whichever one a binding wakes on, it reads a consistent pair.
    void setSession(const QVariant &scope, const QVariant &identity);

signals:
    void stateChanged();
    void scopeChanged();
    void identityChanged();
    void loginRequested(const QString &provider);
    void logoutRequested();

private:
    SynClientConfig m_config;
    QString m_state{QStringLiteral("offline")};
    QVariant m_scope;
    QVariant m_identity; ///< null until authenticated (M8)
    ScopeCheck *m_check{nullptr};
    QJSValue m_checkFunction;
};

} // namespace SynQt

#endif // SYNQT_SESSION_H
