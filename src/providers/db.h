// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_DB_H
#define SYNQT_DB_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>

namespace SynQt {

class IPersistenceProvider;

/// The persistence helper exposed to a database entity's QML as `Db`. It offers only
/// parameterized query/exec and forwards to whichever IPersistenceProvider backs the
/// entity, so a Source never touches a specific engine and can never build SQL by
/// concatenation. A failed statement sets lastError and emits errorOccurred (errors are
/// reported, never thrown across the QML boundary); it never logs credentials, which live
/// only inside the provider.
class Db : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorOccurred)

public:
    explicit Db(IPersistenceProvider *provider, QObject *parent = nullptr);

    /// SELECT: returns one object per row (column name -> value); empty on error.
    Q_INVOKABLE QVariantList query(const QString &sql,
                                   const QVariantList &params = QVariantList());
    /// INSERT/UPDATE/DELETE/DDL: returns { affected, insertId }; empty map on error.
    Q_INVOKABLE QVariantMap exec(const QString &sql,
                                 const QVariantList &params = QVariantList());

    QString lastError() const;

signals:
    void errorOccurred(const QString &message);

private:
    IPersistenceProvider *m_provider;
    QString m_lastError;
};

} // namespace SynQt

#endif // SYNQT_DB_H
