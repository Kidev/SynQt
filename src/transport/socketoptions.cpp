// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "socketoptions.h"

#include <QAbstractSocket>
#include <QVariant>

namespace SynQt {

void disableNagle(QAbstractSocket *socket)
{
    if (!socket) {
        return;
    }
    socket->setSocketOption(QAbstractSocket::LowDelayOption, QVariant{1});
}

} // namespace SynQt
