// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "withdrawsource.h"

#include <QtRemoteObjects/private/qremoteobjectnode_p.h>
#include <QtRemoteObjects/private/qremoteobjectsource_p.h>
#include <QtRemoteObjects/private/qremoteobjectsourceio_p.h>

#include <QHash>
#include <QString>

namespace SynQt {

bool withdrawSource(QRemoteObjectHostBase *node, QObject *source)
{
    const auto *nodePrivate{
        static_cast<const QRemoteObjectHostBasePrivate *>(QObjectPrivate::get(node))};
    QRemoteObjectSourceIo *io{nodePrivate->remoteObjectIo};
    if (io == nullptr) {
        return false;
    }
    const QRemoteObjectRootSource *root{io->m_objectToSourceMap.value(source)};
    if (root == nullptr) {
        return false;
    }

    // Every source QtRO built under this root shares the root's state, which is how they
    // are told apart from another root's. Collected before they are freed.
    QHash<QString, const QRemoteObjectSourceBase *> children;
    for (auto it{io->m_sourceObjects.cbegin()}; it != io->m_sourceObjects.cend(); ++it) {
        if (it.value() != root && it.value()->d == root->d) {
            children.insert(it.key(), it.value());
        }
    }

    if (!node->disableRemoting(source)) {
        return false;
    }

    // Compared by address only: the objects are gone. A name another root has taken since
    // is left alone.
    for (auto it{children.cbegin()}; it != children.cend(); ++it) {
        if (io->m_sourceObjects.value(it.key()) == it.value()) {
            io->m_sourceObjects.remove(it.key());
        }
    }
    return true;
}

} // namespace SynQt
