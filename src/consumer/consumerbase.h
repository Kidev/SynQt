// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CONSUMERBASE_H
#define SYNQT_CONSUMERBASE_H

#include <QList>
#include <QObject>
#include <QString>

namespace SynQt {

/// The base of every generated per-contract consumer facade (`<Contract>Consumer`). The
/// facade is what the accessor family (\qmlServer, Database, ...) actually exposes for a
/// consumed connect point: it forwards the replica's push properties, models and signals,
/// turns a returning slot into a Promise (`slot(args).then(...)`), and publishes itself to
/// the ConnectPointResolver so `<Contract>.on<Signal>` attached handlers can find it.
///
/// The facade is stable across reconnects: the runtime creates it once and calls setReplica()
/// again with the freshly acquired Replica, so QML bindings to the accessor entry stay valid.
/// It is deliberately Replica-type agnostic (it reflects through the metaobject), so the same
/// facade works over a typed Replica on the client and a dynamic Replica on the mesh.
class ConsumerBase : public QObject
{
    Q_OBJECT

public:
    explicit ConsumerBase(QObject *parent = nullptr);
    ~ConsumerBase() override;

    /// The connect-point name this facade serves (the resolver's disambiguating `.point`).
    void setPoint(const QString &point);
    QString point() const;

    /// Bind (or rebind, on reconnect) the underlying Replica. Passing nullptr detaches.
    void setReplica(QObject *replica);
    QObject *replica() const;

    /// True once the bound Replica has completed its QtRO handshake.
    bool isReady() const;

    /// The contract this facade consumes, e.g. "Auth" (the resolver key). Generated.
    virtual QString contractName() const = 0;

signals:
    void readyChanged();

protected:
    /// Wire the property/model/signal relays from m_replica onto this facade's own signals.
    /// Called by setReplica after m_replica/m_dynamic are set; append each connection with
    /// addConnection so it is torn down on the next setReplica. Generated.
    virtual void bindReplica() = 0;

    /// Re-emit every property/model change signal, so bindings reading through the facade
    /// re-evaluate once the Replica is live (or freshly live after a reconnect). Generated.
    virtual void emitAllChanged() = 0;

    void addConnection(const QMetaObject::Connection &connection);

    QObject *m_replica{nullptr};
    bool m_dynamic{false};

private slots:
    void handleInitialized();

private:
    void clearConnections();

    QString m_point;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace SynQt

#endif // SYNQT_CONSUMERBASE_H
