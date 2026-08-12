// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "consumerbase.h"

#include "connectpointresolver.h"
#include "promise.h"

#include <QMetaMethod>
#include <QMetaObject>
#include <QtRemoteObjects/QRemoteObjectReplica>

namespace SynQt {

ConsumerBase::ConsumerBase(QObject *parent)
    : QObject{parent}
{
}

ConsumerBase::~ConsumerBase()
{
    ConnectPointResolver::instance()->retract(this);
}

void ConsumerBase::setPoint(const QString &point)
{
    if (m_point == point) {
        return;
    }
    m_point = point;
    // Published as soon as it is named, not when a Replica arrives. Apps load their QML
    // before the first socket opens, so `<Owner>.on<Signal>` must resolve against a facade
    // with no Replica yet; otherwise the page fails ("Could not create attached properties
    // object"). The facade is the same object either way.
    ConnectPointResolver::instance()->publish(contractName(), m_point, this);
}

QString ConsumerBase::point() const
{
    return m_point;
}

void ConsumerBase::setReplica(QRemoteObjectReplica *replica)
{
    if (m_remote == replica) {
        return;
    }
    // Every answer the old Replica still owed will never come: a call is answered on the
    // connection it was sent on, and a Replica is replaced only when that connection is
    // gone. Reject them, so the failure handlers run and the promises (children of this
    // facade, where the generated forwarders parent them) are freed.
    const QList<QObject *> held{children()};
    for (QObject *child : held) {
        if (Promise *promise{qobject_cast<Promise *>(child)}) {
            promise->abandon(QStringLiteral("the '%1' connect point's link dropped before "
                                            "the answer arrived").arg(m_point));
        }
    }
    clearConnections();
    disconnect(m_initialized);
    m_remote = replica;
    m_replica = replica;
    ConnectPointResolver::instance()->publish(contractName(), m_point, this);
    if (m_remote == nullptr) {
        emit readyChanged();
        return;
    }
    // By member pointer, which reads QRemoteObjectReplica's static metaobject and never
    // asks the Replica for its own.
    m_initialized = connect(m_remote, &QRemoteObjectReplica::initialized, this,
                            &ConsumerBase::handleInitialized);
    // Reconnect fast path: a re-acquired Replica may already be live and will not emit
    // initialized() again.
    if (m_remote->isInitialized()) {
        handleInitialized();
        return;
    }
    emit readyChanged();
}

QObject *ConsumerBase::replica() const
{
    return m_replica;
}

bool ConsumerBase::isReady() const
{
    return m_remote != nullptr && m_remote->isInitialized();
}

void ConsumerBase::handleInitialized()
{
    // Wired again from scratch, so a second initialized() cannot double a relay.
    clearConnections();
    bindReplica();
    emit readyChanged();
    emitAllChanged();
}

bool ConsumerBase::returnsPendingCall(const char *slot) const
{
    if (m_replica == nullptr) {
        return false;
    }
    const QMetaObject *meta{m_replica->metaObject()};
    const QByteArrayView name{slot};
    for (int index{meta->methodOffset()}; index < meta->methodCount(); ++index) {
        const QMetaMethod method{meta->method(index)};
        if (method.name() == name) {
            return QByteArrayView{method.typeName()} == "QRemoteObjectPendingCall";
        }
    }
    return false;
}

void ConsumerBase::addConnection(const QMetaObject::Connection &connection)
{
    m_connections.append(connection);
}

void ConsumerBase::clearConnections()
{
    for (const QMetaObject::Connection &connection : std::as_const(m_connections)) {
        disconnect(connection);
    }
    m_connections.clear();
}

} // namespace SynQt
