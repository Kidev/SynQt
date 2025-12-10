// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "sourcefactory.h"

#include <QHash>
#include <QMetaObject>

#include <utility>

namespace SynQt {

namespace {

QHash<QString, SourceFactory::Factory> &registry()
{
    static QHash<QString, SourceFactory::Factory> factories;
    return factories;
}

} // namespace

void SourceFactory::registerSource(const QString &contract, Factory factory)
{
    if (contract.isEmpty() || !factory) {
        return;
    }
    registry().insert(contract, std::move(factory));
}

QObject *SourceFactory::create(const QString &contract, QObject *parent)
{
    const auto factory{registry().constFind(contract)};
    if (factory == registry().constEnd()) {
        return nullptr;
    }
    return (*factory)(parent);
}

bool SourceFactory::mirror(QObject *source, QObject *shared, QObject *caller)
{
    if (!source || !shared) {
        return false;
    }
    // The caller first. A mirror that forwarded a call before it knew whose it was would
    // hand the shared Source the previous caller's identity.
    if (!bindCaller(source, caller)) {
        return false;
    }
    return QMetaObject::invokeMethod(source, "synqtMirror", Qt::DirectConnection,
                                     Q_ARG(QObject *, shared));
}

bool SourceFactory::bindCaller(QObject *source, QObject *caller)
{
    if (!source) {
        return false;
    }
    return QMetaObject::invokeMethod(source, "synqtSetCaller", Qt::DirectConnection,
                                     Q_ARG(QObject *, caller));
}

bool SourceFactory::relay(QObject *source, QObject *behind)
{
    if (!source || !behind) {
        return false;
    }
    return QMetaObject::invokeMethod(source, "synqtRelay", Qt::DirectConnection,
                                     Q_ARG(QObject *, behind));
}

bool SourceFactory::holdsSharedState(QObject *source)
{
    if (!source) {
        return false;
    }
    return QMetaObject::invokeMethod(source, "synqtHoldsSharedState", Qt::DirectConnection);
}

} // namespace SynQt
