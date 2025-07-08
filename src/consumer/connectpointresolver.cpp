// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "connectpointresolver.h"

namespace SynQt {

ConnectPointResolver::ConnectPointResolver(QObject *parent)
    : QObject{parent}
{
}

ConnectPointResolver *ConnectPointResolver::instance()
{
    static ConnectPointResolver resolver;
    return &resolver;
}

void ConnectPointResolver::publish(const QString &contract, const QString &point,
                                   QObject *consumer)
{
    m_entries[contract].insert(point, consumer);
    emit changed(contract);
}

void ConnectPointResolver::retract(QObject *consumer)
{
    QStringList touched;
    for (auto contractIt{m_entries.begin()}; contractIt != m_entries.end(); ++contractIt) {
        QMap<QString, QObject *> &points{contractIt.value()};
        for (auto pointIt{points.begin()}; pointIt != points.end();) {
            if (pointIt.value() == consumer) {
                pointIt = points.erase(pointIt);
                touched.append(contractIt.key());
            } else {
                ++pointIt;
            }
        }
    }
    for (const QString &contract : std::as_const(touched)) {
        emit changed(contract);
    }
}

QObject *ConnectPointResolver::resolve(const QString &contract, const QString &point) const
{
    const auto contractIt{m_entries.constFind(contract)};
    if (contractIt == m_entries.constEnd() || contractIt.value().isEmpty()) {
        return nullptr;
    }
    const QMap<QString, QObject *> &points{contractIt.value()};
    if (point.isEmpty()) {
        return points.first();
    }
    return points.value(point, nullptr);
}

QStringList ConnectPointResolver::points(const QString &contract) const
{
    const auto contractIt{m_entries.constFind(contract)};
    if (contractIt == m_entries.constEnd()) {
        return {};
    }
    return contractIt.value().keys();
}

} // namespace SynQt
