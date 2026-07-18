// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "consumerbase.h"

#include "connectpointresolver.h"

#include <QtRemoteObjects/QRemoteObjectDynamicReplica>
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
    if (m_replica != nullptr) {
        ConnectPointResolver::instance()->publish(contractName(), m_point, this);
    }
}

QString ConsumerBase::point() const
{
    return m_point;
}

void ConsumerBase::setReplica(QObject *replica)
{
    if (m_replica == replica) {
        return;
    }
    clearConnections();
    m_replica = replica;
    m_dynamic = (qobject_cast<QRemoteObjectDynamicReplica *>(replica) != nullptr);
    if (m_replica != nullptr) {
        addConnection(connect(m_replica, SIGNAL(initialized()), this,
                              SLOT(handleInitialized())));
        bindReplica();
    }
    ConnectPointResolver::instance()->publish(contractName(), m_point, this);
    emit readyChanged();
    // Reconnect fast path: a re-acquired Replica may already be live, in which case no
    // further initialized() will fire, so surface its state now.
    QRemoteObjectReplica *asReplica{qobject_cast<QRemoteObjectReplica *>(m_replica)};
    if (asReplica != nullptr && asReplica->isInitialized()) {
        emitAllChanged();
    }
}

QObject *ConsumerBase::replica() const
{
    return m_replica;
}

bool ConsumerBase::isReady() const
{
    QRemoteObjectReplica *asReplica{qobject_cast<QRemoteObjectReplica *>(m_replica)};
    return asReplica != nullptr && asReplica->isInitialized();
}

void ConsumerBase::handleInitialized()
{
    emit readyChanged();
    emitAllChanged();
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
