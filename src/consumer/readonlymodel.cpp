// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "readonlymodel.h"

namespace SynQt {

ReadOnlyModel::ReadOnlyModel(QObject *parent)
    : QIdentityProxyModel{parent}
{
}

bool ReadOnlyModel::setData(const QModelIndex &, const QVariant &, int)
{
    return false;
}

Qt::ItemFlags ReadOnlyModel::flags(const QModelIndex &index) const
{
    return QIdentityProxyModel::flags(index) & ~Qt::ItemIsEditable;
}

} // namespace SynQt
