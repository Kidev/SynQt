// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SESSION_H
#define SYNQT_SESSION_H

#include "synclientconfig.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace SynQt {

/// Read-only session state plus the two actions that change it (docs/runtime-api.md).
/// It never exposes a secret: the raw session id and any token live at the edge, not in
/// the client. The framework, not app code, drives state/scope/identity; login/logout
/// are surfaced to QML.
class Session : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QVariant scope READ scope NOTIFY scopeChanged)
    Q_PROPERTY(QVariant identity READ identity NOTIFY identityChanged)
    Q_PROPERTY(bool isAuthenticated READ isAuthenticated NOTIFY identityChanged)

public:
    explicit Session(SynClientConfig config, QObject *parent = nullptr);

    QString state() const;
    QVariant scope() const;
    QVariant identity() const;
    bool isAuthenticated() const;

    Q_INVOKABLE bool hasScope(const QString &name) const;
    Q_INVOKABLE void login(const QString &provider = QString());
    Q_INVOKABLE void logout();

    /// Framework-side setters (driven by SynClient / the edge), not app-facing.
    void setState(const QString &state);
    void setScope(const QVariant &scope);
    void setIdentity(const QVariant &identity);

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
};

} // namespace SynQt

#endif // SYNQT_SESSION_H
