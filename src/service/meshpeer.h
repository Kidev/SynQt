// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MESHPEER_H
#define SYNQT_MESHPEER_H

#include <QMetaType>
#include <QString>

namespace SynQt {

/// The identity of the entity on the far end of a mesh link, as the transport can
/// vouch for it. On a mutual-TLS link `entity` is the verified peer certificate's
/// subject and `authenticated` is true. On an opt-in local socket the OS identifies
/// the connecting user but not the entity, so `entity` is trusted by colocation and
/// `authenticated` is false: any same-user process could present it. Authorization
/// (M4/M7) must treat an unauthenticated peer accordingly and must never conflate it
/// with a certificate-verified one.
struct MeshPeer
{
    QString entity;
    bool authenticated{false};
};

} // namespace SynQt

Q_DECLARE_METATYPE(SynQt::MeshPeer)

#endif // SYNQT_MESHPEER_H
