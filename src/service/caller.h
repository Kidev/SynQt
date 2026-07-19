// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CALLER_H
#define SYNQT_CALLER_H

#include "sessionmanager.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

namespace SynQt {

/// The identity of whoever is calling the owner's slot, exposed to the owner QML as the
/// context property \qmlCaller (and, for browser callers, aliased as \qmlClient). Bound to
/// one per-connection Source instance: a per_session instance carries a browser user
/// (isUser), a per_peer instance carries a verified calling entity (isEntity). The two
/// identity systems never mix; a user value is never treated as an entity, and vice versa.
///
/// User callers expose session/identity/scope/hasScope/setScope; entity callers expose the
/// certificate-verified entity name. emitSignal delivers a contract signal to this one
/// caller (the per-connection instance has a single consumer, so emitting on it targets the
/// caller alone).
///
/// \sa \ref qmlcaller "the Caller accessor page", \ref qmlclient "the Client alias"
class Caller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isUser READ isUser CONSTANT)
    Q_PROPERTY(bool isEntity READ isEntity CONSTANT)
    Q_PROPERTY(bool isEntityVerified READ isEntityVerified CONSTANT)
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QVariant session READ session CONSTANT)
    Q_PROPERTY(QVariant identity READ identity CONSTANT)
    Q_PROPERTY(QString scope READ scope CONSTANT)
    Q_PROPERTY(QString entity READ entity CONSTANT)

public:
    /// Builds a Caller for a connect point's contract. A contract with a generated
    /// `\<Contract\>Caller` factory (registered by `synqtRegister\<Contract\>Sources`) yields that
    /// subclass, so QML gets the typed Caller.emit<Signal>(...) sugar; an unknown or empty
    /// contract yields the base Caller (emitSignal still works). Public API only.
    using CallerFactory = std::function<Caller *(QObject *)>;
    static void registerCallerFactory(const QString &contract, CallerFactory factory);

    /// A browser user caller, backed by a live session. The manager is read live so
    /// setScope and login elevation are reflected without rebuilding the Caller. `contract`
    /// selects the typed subclass for the emit<Signal> sugar (empty for the scope gate).
    static Caller *forUser(const QString &contract, SessionManager *sessions,
                           const QByteArray &sessionId, QObject *source,
                           QObject *parent = nullptr);
    /// A calling entity reached over the mesh. `verified` is true when the transport
    /// authenticated it (mutual TLS: certificate subject == name); false when the name is
    /// trusted only by colocation (the opt-in local socket, where the OS confirms the peer's
    /// user but any same-user process could present any entity name). An owner that gates on
    /// entity identity for a privileged action must require isEntityVerified, never isEntity
    /// alone, on a topology that permits a local link.
    static Caller *forEntity(const QString &contract, const QString &entityName, bool verified,
                             QObject *source, QObject *parent = nullptr);

    bool isUser() const;
    bool isEntity() const;
    bool isEntityVerified() const; ///< entity: certificate-verified; colocation-trusted: false
    QString id() const;
    QVariant session() const;  ///< user: {id, scope, identity}; entity: null
    QVariant identity() const; ///< user: {sub, login, name, email} or null; entity: null
    QString scope() const;     ///< user's granted scope; entity: empty
    QString entity() const;    ///< verified entity name; user: empty

    Q_INVOKABLE bool hasScope(const QString &scope) const;
    Q_INVOKABLE void setScope(const QString &scope,
                              const QVariantMap &identity = QVariantMap());

    /// Deliver a contract signal to this caller by invoking the Source helper's generated
    /// emit<Signal> method (e.g. emitSignal("rejected", reason)). Positional arguments keep
    /// the QML call site unambiguous; up to four are supported. The typed sugar
    /// Caller.emit<Signal>(...) (e.g. Caller.emitRejected(reason)) is a thin forwarder on
    /// the generated `\<Contract\>Caller` subclass that calls straight into this.
    Q_INVOKABLE void emitSignal(const QString &signalName,
                                const QVariant &arg0 = QVariant(),
                                const QVariant &arg1 = QVariant(),
                                const QVariant &arg2 = QVariant(),
                                const QVariant &arg3 = QVariant());

    /// The scope vocabulary for hierarchical checks (order low->high). Empty == set-based.
    void setScopeOrder(const QStringList &order, bool hierarchical);

    /// Bind the Source whose emit<Signal> methods emitSignal drives. Set after the Source
    /// is created, since the Source's QML context needs the Caller first.
    void setSource(QObject *source);

protected:
    /// The generated `\<Contract\>Caller` subclass constructs through this; its typed
    /// emit<Signal>(...) methods forward to the inherited emitSignal.
    explicit Caller(QObject *parent);

private:
    /// Build the Caller for a contract: its registered `\<Contract\>Caller`, or the base Caller.
    /// A member, so it may reach the protected base constructor.
    static Caller *create(const QString &contract, QObject *parent);

    const SessionRecord *record() const;

    QPointer<SessionManager> m_sessions;
    QByteArray m_sessionId;
    QString m_entity;
    QPointer<QObject> m_source;
    QStringList m_scopeOrder;
    bool m_isUser{false};
    bool m_entityVerified{false};
    bool m_hierarchical{true};
};

} // namespace SynQt

#endif // SYNQT_CALLER_H
