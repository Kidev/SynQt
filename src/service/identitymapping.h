// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IDENTITYMAPPING_H
#define SYNQT_IDENTITYMAPPING_H

#include <QObject>

namespace SynQt {

/// The base type the identity mapping hook (web/identity/map.qml) derives from. The hook
/// adds `function scopeFor(identity)`; the edge invokes it after a successful login to turn
/// a normalized identity into a SynQt scope. It runs only on the edge.
class IdentityMapping : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
};

} // namespace SynQt

#endif // SYNQT_IDENTITYMAPPING_H
