// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "sessionstatesource.h"

#include "caller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QVariantMap>

namespace SynQt {

namespace {
constexpr auto kScope{"scope"};
constexpr auto kIdentity{"identity"};
}

SessionStateSource::SessionStateSource(Caller *caller, QObject *parent)
    : SessionStateSimpleSource{parent}
    , m_caller{caller}
{
    // Published now: the client reads it first, and the session already has its scope and
    // identity, so no later event would announce them.
    publish();
    // A rotation is the only thing that changes a live session's privileges (usually
    // Caller.setScope in a slot). The Caller has already followed the rotation when this
    // runs.
    connect(m_caller, &Caller::scopeChanged, this, &SessionStateSource::publish);
}

void SessionStateSource::publish()
{
    // No session for this connection: leave the property unset. An empty scope would make
    // the client drop even its default scope.
    if (!m_caller->hasSession()) {
        return;
    }
    QJsonObject state;
    state.insert(QLatin1String{kScope}, m_caller->scope());
    // Null, not an empty object, while anonymous: `Session.identity` is documented as null,
    // and an empty object is truthy in `!Session.identity`.
    const QVariantMap identity{m_caller->identity().toMap()};
    state.insert(QLatin1String{kIdentity},
                 identity.isEmpty() ? QJsonValue{QJsonValue::Null}
                                    : QJsonValue{QJsonObject::fromVariantMap(identity)});
    setSession(QString::fromUtf8(
        QJsonDocument{state}.toJson(QJsonDocument::Compact)));
}

} // namespace SynQt
