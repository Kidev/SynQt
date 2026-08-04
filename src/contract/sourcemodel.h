// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QStandardItemModel>

namespace SynQt {

/// The model a contract publishes. Owner to consumer only.
///
/// QtRO exposes a remoted model's setData() to consumers as a slot on the wire. A write
/// arriving that way lands in the owner's model without passing through the owner's QML,
/// so no Caller and no owner-side rule ever sees it, and the result fans out to every
/// other consumer. This model refuses it. A consumer that wants owner state changed calls
/// a slot, where the Caller exists and the owner decides.
///
/// The owner publishes through QStandardItem::setData, which does not go through here.
class SourceModel : public QStandardItemModel
{
    Q_OBJECT

public:
    explicit SourceModel(QObject *parent = nullptr);

    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
};

} // namespace SynQt
