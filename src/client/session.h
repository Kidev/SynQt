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

/// Read-only session state plus the two actions that change it (see the
/// [runtime API reference](https://synqt.org/runtime-api/)). It holds no secret: the session
/// id and any token stay at the edge. The framework drives state, scope and identity.
///
/// \sa \ref qmlsession "the Session accessor page"
class Session : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QVariant scope READ scope NOTIFY scopeChanged)
    Q_PROPERTY(QVariant identity READ identity NOTIFY identityChanged)
    Q_PROPERTY(bool isAuthenticated READ isAuthenticated NOTIFY identityChanged)
    /// The scope check, as a property rather than a Q_INVOKABLE, so a binding such as
    /// `visible: !Session.hasScope("player")` re-evaluates whenever the scope changes.
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

    /// Both at once, as the edge sends them: both members are written before either signal goes
    /// out, so a binding never reads a new scope with an old identity.
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
    QVariant m_identity; ///< null until authenticated
    QJSValue m_checkFunction; ///< `hasScope`, built in the constructor. See the property
};

} // namespace SynQt

#endif // SYNQT_SESSION_H
