// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CONSUMERBASE_H
#define SYNQT_CONSUMERBASE_H

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

QT_BEGIN_NAMESPACE
class QJSEngine;
class QRemoteObjectReplica;
QT_END_NAMESPACE

namespace SynQt {

/// The base of every generated per-contract consumer facade (`<Contract>Consumer`). The
/// facade is what the accessor family (\qmlServer, Database, ...) exposes for a
/// consumed connect point. It forwards the replica's push properties, models and signals,
/// turns a returning slot into a Promise (`slot(args).then(...)`), and publishes itself to
/// the ConnectPointResolver so `<Contract>.on<Signal>` attached handlers can find it.
///
/// The facade is stable across reconnects: the runtime creates it once and calls setReplica()
/// again with the freshly acquired Replica, so QML bindings to the accessor entry stay valid.
/// It reflects through the metaobject rather than naming a Replica type, so the same facade
/// works over a typed Replica on the client and a dynamic Replica on the mesh.
class ConsumerBase : public QObject
{
    Q_OBJECT

public:
    explicit ConsumerBase(QObject *parent = nullptr);
    ~ConsumerBase() override;

    /// The connect point this facade serves, which is its owner's name.
    void setPoint(const QString &point);
    QString point() const;

    /// Bind (or rebind, on reconnect) the underlying Replica. Passing nullptr detaches.
    /// The relays are wired once the Replica is initialized: a dynamic Replica has no API
    /// before that, and asking it for its metaobject then is an error.
    void setReplica(QRemoteObjectReplica *replica);
    QObject *replica() const;

    /// True once the bound Replica has completed its QtRO handshake.
    bool isReady() const;

    /// The engine a returning slot's Promise hands its answer to QML through. Set by
    /// whoever puts the facade in QML scope. A call made through the attached type
    /// (`Books.recentWinners()`) gives the facade no JavaScript wrapper, so qjsEngine()
    /// alone can return null, and a null engine resolves every answer as undefined.
    void setJsEngine(QJSEngine *engine);
    QJSEngine *jsEngine() const;

    /// The contract this facade consumes, e.g. "Auth" (the resolver key). Generated.
    virtual QString contractName() const = 0;

signals:
    void readyChanged();

protected:
    /// Wire the property/model/signal relays from m_replica onto this facade's own signals.
    /// Called once the bound Replica is initialized. Append each connection with
    /// addConnection so it is torn down on the next setReplica. Generated.
    virtual void bindReplica() = 0;

    /// Re-emit every property/model change signal, so bindings reading through the facade
    /// re-evaluate once the Replica is live (or freshly live after a reconnect). Generated.
    virtual void emitAllChanged() = 0;

    void addConnection(const QMetaObject::Connection &connection);

    /// Whether the bound Replica declares the named slot as returning
    /// QRemoteObjectPendingCall, as a dynamic Replica does, rather than
    /// QRemoteObjectPendingReply<T>. Asked only once isReady().
    bool returnsPendingCall(const char *slot) const;

    /// The bound Replica, as the QObject the generated members reflect on. Read through it
    /// only when isReady(), since a dynamic Replica has no members before that.
    QObject *m_replica{nullptr};

private:
    void handleInitialized();
    void clearConnections();

    QString m_point;
    QPointer<QJSEngine> m_jsEngine;
    QRemoteObjectReplica *m_remote{nullptr};
    QMetaObject::Connection m_initialized;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace SynQt

#endif // SYNQT_CONSUMERBASE_H
