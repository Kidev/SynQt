// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QIdentityProxyModel>

namespace SynQt {

/// The consumer side of a published model, which is read and never written.
///
/// The owner refuses a write that reaches it (`SynQt::SourceModel`). This closes the local
/// half: `QAbstractItemModelReplica::setData` writes the replica's own cache and returns true
/// before anything crosses the wire, so a consumer would show a value nobody accepted until
/// the next publish replaced it.
///
/// `flags()` clears the editable bit so a view offers no editor, and `setData` refuses a
/// caller that reaches the model directly without asking about flags.
class ReadOnlyModel : public QIdentityProxyModel
{
    Q_OBJECT

public:
    explicit ReadOnlyModel(QObject *parent = nullptr);

    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
};

} // namespace SynQt
