// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CONNECTPOINTRESOLVER_H
#define SYNQT_CONNECTPOINTRESOLVER_H

#include <QHash>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

namespace SynQt {

/// The bridge from a contract name to the live consumer facade for a connect point this
/// entity consumes. A `<Contract>.on<Signal>` attached handler has no `target`: it resolves
/// the connect point it consumes for that contract through this registry. Consumer facades
/// publish themselves here as they bind a replica (and re-publish on reconnect); the attached
/// type resolves by contract, optionally disambiguated by the connect point's name (`.point`).
///
/// Process-global (one entity runs one process): the accessor family (\qmlServer,
/// Database, ...) and the attached types share one view of what is currently consumed.
class ConnectPointResolver : public QObject
{
    Q_OBJECT

public:
    static ConnectPointResolver *instance();

    /// Register (or refresh) the facade that serves `contract` at connect point `point`.
    void publish(const QString &contract, const QString &point, QObject *consumer);

    /// Drop a facade (called when it is destroyed).
    void retract(QObject *consumer);

    /// The facade for `contract`. When `point` is empty, the sole consumed connect point of
    /// that contract is returned (the first, if several exist and none was named). Null when
    /// this entity consumes no connect point of the contract.
    QObject *resolve(const QString &contract, const QString &point = QString{}) const;

    /// The connect-point names currently consumed for `contract`.
    QStringList points(const QString &contract) const;

signals:
    /// A facade for `contract` was published or retracted, so attached handlers rebind.
    void changed(const QString &contract);

private:
    explicit ConnectPointResolver(QObject *parent = nullptr);

    /// contract -> (point -> facade). QMap keeps a stable "first" for the empty-point lookup.
    QHash<QString, QMap<QString, QObject *>> m_entries;
};

} // namespace SynQt

#endif // SYNQT_CONNECTPOINTRESOLVER_H
