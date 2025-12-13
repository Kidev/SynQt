// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QIdentityProxyModel>

namespace SynQt {

/// The consumer side of a published model, which is a thing to read and never to write.
///
/// The owner already refuses a write that reaches it (`SynQt::SourceModel`), so nothing a
/// consumer does here can change what anybody else sees. What this closes is the half the
/// owner cannot reach: `QAbstractItemModelReplica::setData` writes the replica's own cache
/// and returns true before anything crosses the wire, so a consumer that called it saw its
/// own value accepted, and went on showing it until the next publish replaced it. Nobody
/// else was ever affected, which is exactly what makes it worth closing: a lie a view tells
/// only to itself is the kind that survives review.
///
/// Both halves are needed and only one of them is load bearing. `flags()` is what a view
/// consults before it offers an editor, so clearing the editable bit is what stops a
/// delegate opening one; `setData` is what stops everything else, because a caller reaching
/// the model directly never asks about flags.
class ReadOnlyModel : public QIdentityProxyModel
{
    Q_OBJECT

public:
    explicit ReadOnlyModel(QObject *parent = nullptr);

    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
};

} // namespace SynQt
