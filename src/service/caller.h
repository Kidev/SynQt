// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CALLER_H
#define SYNQT_CALLER_H

#include "sessionmanager.h"
#include "tracecontext.h"

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
/// one Source instance. A Source minted for a browser session carries a user (isUser), one
/// minted for a mesh peer carries a verified calling entity (isEntity). The two identity
/// systems never mix. A user value is never treated as an entity, and vice versa.
///
/// User callers expose session/identity/scope/hasScope/setScope. Entity callers expose the
/// certificate-verified entity name. emitSignal delivers a contract signal to this one
/// caller (the Source is that caller's, so emitting on it targets the caller alone).
///
/// \sa \ref qmlcaller "the Caller accessor page", \ref qmlclient "the Client alias"
class Caller : public QObject
{
    Q_OBJECT
    // All nine notify on one signal, and none is CONSTANT: `adopt` re-points a Caller at the
    // current caller and an elevation rotates its session, so a binding on `Caller.scope` must
    // be told.
    Q_PROPERTY(bool isUser READ isUser NOTIFY callerChanged)
    Q_PROPERTY(bool isEntity READ isEntity NOTIFY callerChanged)
    Q_PROPERTY(bool isEntityVerified READ isEntityVerified NOTIFY callerChanged)
    Q_PROPERTY(bool hasSession READ hasSession NOTIFY callerChanged)
    Q_PROPERTY(QString id READ id NOTIFY callerChanged)
    Q_PROPERTY(QVariant session READ session NOTIFY callerChanged)
    Q_PROPERTY(QVariant identity READ identity NOTIFY callerChanged)
    Q_PROPERTY(QString scope READ scope NOTIFY callerChanged)
    Q_PROPERTY(QString entity READ entity NOTIFY callerChanged)

public:
    /// Builds a Caller for a connect point's contract. A contract with a generated
    /// `\<Contract\>Caller` factory (registered by `synqtRegister\<Contract\>Sources`) yields that
    /// subclass, so QML gets the typed Caller.emit<Signal>(...) sugar. An unknown or empty
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
    /// authenticated it (mutual TLS: certificate subject == name). False when the name is
    /// trusted only by colocation (the opt-in local socket, where the OS confirms the peer's
    /// user but any same-user process could present any entity name). An owner that gates on
    /// entity identity for a privileged action must require isEntityVerified, never isEntity
    /// alone, on a topology that permits a local link.
    static Caller *forEntity(const QString &contract, const QString &entityName, bool verified,
                             QObject *source, QObject *parent = nullptr);

    bool isUser() const;
    bool isEntity() const;
    bool isEntityVerified() const; ///< entity: certificate-verified. Colocation-trusted: false
    bool hasSession() const;   ///< is there a session behind this call, either way it arrived
    QString id() const;
    QVariant session() const;  ///< {key, scope, identity} and, on the edge, the id. Else null
    QVariant identity() const; ///< {sub, login, name, email}, or null when anonymous
    QString scope() const;     ///< the granted scope, empty when there is no session
    QString entity() const;    ///< verified entity name. User: empty

    Q_INVOKABLE bool hasScope(const QString &scope) const;
    Q_INVOKABLE void setScope(const QString &scope,
                              const QVariantMap &identity = QVariantMap());

    /// The number of arguments a contract signal may carry to one caller. Bounded because
    /// each one is a defaulted parameter of the Q_INVOKABLE below and QMetaObject::invokeMethod
    /// takes a fixed argument pack. synqtc refuses a longer signal by name, rather than
    /// letting it become an unreadable template error in generated code.
    static constexpr int MaxSignalArgs{8};

    /// Deliver a contract signal to this caller by invoking the Source helper's generated
    /// emit<Signal> method (e.g. emitSignal("rejected", reason)). Positional arguments keep
    /// the QML call site unambiguous. Up to MaxSignalArgs are supported. The typed sugar
    /// Caller.emit<Signal>(...) (e.g. Caller.emitRejected(reason)) is a thin forwarder on
    /// the generated `\<Contract\>Caller` subclass that calls straight into this.
    Q_INVOKABLE void emitSignal(const QString &signalName,
                                const QVariant &arg0 = QVariant(),
                                const QVariant &arg1 = QVariant(),
                                const QVariant &arg2 = QVariant(),
                                const QVariant &arg3 = QVariant(),
                                const QVariant &arg4 = QVariant(),
                                const QVariant &arg5 = QVariant(),
                                const QVariant &arg6 = QVariant(),
                                const QVariant &arg7 = QVariant());

    /// Become `other`: whoever it identifies, and the Source it answers through.
    ///
    /// A shared entity answers everyone from one Source, so its Caller is re-pointed at the
    /// caller of the slot about to run. On a shared entity `Caller` therefore means "whoever is
    /// calling right now": read it in the slot and keep what later work needs in a local.
    /// Invokable because the generated helper reaches it by name.
    Q_INVOKABLE void adopt(QObject *other);

    /// The session to hand a downstream entity, empty when there is none: the session's key, its
    /// scope and its identity, never the browser's credential, which stays at the edge. A mesh
    /// slot call carries it so every entity down the chain knows whom the call is for. Reached by
    /// name from SynQt::ActingFor.
    Q_INVOKABLE QVariantMap forwardedSession() const;

    /// Take the session the calling entity says it is acting for.
    ///
    /// Honored only for an entity caller; a browser's Caller ignores it. Between entities it is
    /// trusted exactly as far as the peer's certificate. The caller stays an entity (\ref isUser
    /// is still false). Called on every mesh slot call, an empty map included, so a reused Caller
    /// never keeps the last call's session.
    Q_INVOKABLE void assumeSession(const QVariantMap &session);

    /// The span the calling entity says its call continues, invalid when it said nothing. Taken
    /// by \ref assumeSession on a mesh link only, when both identifiers have the tracer's shape
    /// (TraceContext::fromWire), so one click is one trace across entities. A browser's Caller
    /// never holds one.
    TraceContext traceContext() const;
    void setTraceContext(const TraceContext &context);

    /// The scope vocabulary for hierarchical checks (order low->high). Empty == set-based.
    void setScopeOrder(const QStringList &order, bool hierarchical);

    /// Bind the Source whose emit<Signal> methods emitSignal drives. Set after the Source
    /// is created, since the Source's QML context needs the Caller first.
    void setSource(QObject *source);

Q_SIGNALS:
    /// What this caller is authorized for has changed, because a scope change rotated its
    /// session. A generated Source republishes its `\<scope\>` gated members on it.
    void scopeChanged();

    /// Every property above may now answer differently: after an elevation, or after `adopt`
    /// re-points the Caller. Emitted only when something an accessor reads moved.
    void callerChanged();

protected:
    /// The generated `\<Contract\>Caller` subclass constructs through this. Its typed
    /// emit<Signal>(...) methods forward to the inherited emitSignal.
    explicit Caller(QObject *parent);

private:
    /// Build the Caller for a contract. Its registered `\<Contract\>Caller`, or the base Caller.
    /// A member, so it may reach the protected base constructor.
    static Caller *create(const QString &contract, QObject *parent);

    const SessionRecord *record() const;

    QPointer<SessionManager> m_sessions;
    QByteArray m_sessionId;
    /// The session a calling entity said it was acting for, empty when it said nothing.
    /// Read only through the accessors below, which prefer a live session of this edge's
    /// own over any assertion, so a user caller can never be talked into being someone else.
    QVariantMap m_forwarded;
    /// The trace this call continues. Kept beside the forwarded session rather than in
    /// it, so that map stays exactly the three keys a session is made of. What travels
    /// on from here is not this but the span opened for the call. See SynQt::ActingFor.
    TraceContext m_trace;
    QString m_entity;
    QPointer<QObject> m_source;
    QStringList m_scopeOrder;
    bool m_isUser{false};
    bool m_entityVerified{false};
    bool m_hierarchical{true};
};

} // namespace SynQt

#endif // SYNQT_CALLER_H
