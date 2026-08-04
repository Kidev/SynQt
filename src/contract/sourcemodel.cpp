// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "sourcemodel.h"

namespace SynQt {

SourceModel::SourceModel(QObject *parent)
    : QStandardItemModel{parent}
{
}

bool SourceModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    Q_UNUSED(index);
    Q_UNUSED(value);
    Q_UNUSED(role);
    return false;
}

Qt::ItemFlags SourceModel::flags(const QModelIndex &index) const
{
    // Clearing the flag is what makes a QML delegate's `model.field = x` fail where it is
    // written instead of travelling. It is not what closes the path: QStandardItemModel
    // does not consult flags in setData, so the override above is the one that does.
    return QStandardItemModel::flags(index) & ~Qt::ItemIsEditable;
}

} // namespace SynQt
