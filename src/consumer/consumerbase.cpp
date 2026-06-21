// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "consumerbase.h"

#include "connectpointresolver.h"
#include "promise.h"

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

void ConsumerBase::setReplica(QObject *replica)
{
    if (m_replica == replica) {
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
    m_replica = replica;
    m_dynamic = (qobject_cast<QRemoteObjectDynamicReplica *>(replica) != nullptr);
    if (m_replica != nullptr) {
        addConnection(connect(m_replica, SIGNAL(initialized()), this,
                              SLOT(handleInitialized())));
        bindReplica();
    }
    ConnectPointResolver::instance()->publish(contractName(), m_point, this);
    emit readyChanged();
    // Reconnect fast path: a re-acquired Replica may already be live and will not emit
    // initialized() again.
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
    if (m_dynamic) {
        // A dynamic Replica has no API until it is initialized, so relays wired at bind
        // time found nothing to connect to. They are wired again here, and the connection
        // this slot came in on is remade with them.
        clearConnections();
        addConnection(connect(m_replica, SIGNAL(initialized()), this,
                              SLOT(handleInitialized())));
        bindReplica();
    }
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
