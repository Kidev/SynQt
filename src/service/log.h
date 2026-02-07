// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_LOG_H
#define SYNQT_LOG_H

#include <QObject>
#include <QVariantMap>

namespace SynQt {

/// What an entity has to say about itself, in its own QML: `Log.info("started", {items: 3})`.
///
/// The framework records what it can see (links coming up, callers refused, calls crossing);
/// this is where an entity records why it did what it did.
///
/// Installed for every type, unlike `Db` or `Cache`, which exist only where a type has an
/// engine behind it.
///
/// A message and a map, not a formatted sentence: write `Log.info("saved rows", {rows: count})`
/// so the record can be filtered and searched.
///
/// The runtime stamps the entity name on the way out, and an `entity` key in the attributes is
/// not read, so an entity cannot log as another.
class Log : public QObject
{
    Q_OBJECT

public:
    explicit Log(QObject *parent = nullptr);

    Q_INVOKABLE void debug(const QString &message,
                           const QVariantMap &attributes = QVariantMap());
    Q_INVOKABLE void info(const QString &message,
                          const QVariantMap &attributes = QVariantMap());
    Q_INVOKABLE void warn(const QString &message,
                          const QVariantMap &attributes = QVariantMap());
    Q_INVOKABLE void error(const QString &message,
                           const QVariantMap &attributes = QVariantMap());
};

} // namespace SynQt

#endif // SYNQT_LOG_H
