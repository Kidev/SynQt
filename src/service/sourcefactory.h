// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SOURCEFACTORY_H
#define SYNQT_SOURCEFACTORY_H

#include <QObject>
#include <QString>

#include <functional>

namespace SynQt {

/// How the runtime builds one more instance of a generated Source when it knows only the
/// contract's name: the per-caller mirrors of a shared entity. A mirror is built from the C++
/// helper class, never by loading the QML again, and `synqtRegister<Stem>Sources()`
/// registers the way to do it.
class SourceFactory
{
public:
    using Factory = std::function<QObject *(QObject *parent)>;

    /// Register how to build a Source for `contract`. Called by generated code. The last
    /// registration for a name wins, which is what makes a second call harmless.
    static void registerSource(const QString &contract, Factory factory);

    /// Build one, or nullptr when no contract of that name was registered.
    static QObject *create(const QString &contract, QObject *parent);

    /// Turn `source` into a mirror of `shared` answering for `caller`. Both steps go
    /// through the meta-object by name, because the helper's type is exactly what is not
    /// known here. Returns false when the object does not answer them.
    static bool mirror(QObject *source, QObject *shared, QObject *caller);

    /// Tell a Source whose caller it answers, without mirroring anything. This is the
    /// shared Source itself. The Caller in its QML context is the one a mirror's forwarded
    /// call adopts into.
    static bool bindCaller(QObject *source, QObject *caller);

    /// Point `source` at the entity answering for it, making it one caller's view of a connect
    /// point its entity does not implement: a front. `behind` is a Replica of that entity's own
    /// point; everything it publishes is followed outward and every slot forwarded back with the
    /// caller's session. Returns false when the object does not support it.
    ///
    /// Calling it again follows the new object; a null `behind` follows nothing. A caller's
    /// scope change and a mesh reconnect both need this.
    static bool relay(QObject *source, QObject *behind);

    /// Mark this Source as the one a shared entity answers everyone from, so its `\<scope\>`
    /// gated members are not gated on it; each mirror applies its caller's gate. Every other
    /// Source gates by default. Returns false when the object does not support it (a contract
    /// with no gated member).
    static bool holdsSharedState(QObject *source);
};

} // namespace SynQt

#endif // SYNQT_SOURCEFACTORY_H
