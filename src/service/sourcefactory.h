// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SOURCEFACTORY_H
#define SYNQT_SOURCEFACTORY_H

#include <QObject>
#include <QString>

#include <functional>

namespace SynQt {

/// How the runtime builds one more instance of a generated Source when it knows only the
/// contract's name.
///
/// It needs one for a shared entity. That entity answers everyone from a single Source
/// loaded from its QML, and every caller reaches it through a mirror. A plain instance of
/// the same generated helper, carrying that caller's Caller, republishing what the shared
/// Source pushes and forwarding what the caller asks. Loading the QML file again would
/// give the wrong thing (a second Source, with the file's own timers and bindings running
/// a second time), so the mirror is built from the C++ helper class instead, and the
/// generated `synqtRegister<Stem>Sources()` is what registers a way to do that.
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
    /// shared Source itself: the Caller in its QML context is the one a mirror's forwarded
    /// call adopts into.
    static bool bindCaller(QObject *source, QObject *caller);

    /// Point `source` at the entity answering for it, making it one caller's view of a
    /// connect point its own entity does not implement.
    ///
    /// This is what a front is: a web edge owns a point, holds the session and runs the
    /// sign-in, and hands each caller to the entity serving people of their scope. The
    /// Source the browser acquires is `source` and `behind` is a Replica of that entity's
    /// own point; everything it publishes is followed outward and every slot is forwarded
    /// back, carrying the session the call is being made for. Returns false when the object
    /// does not answer it.
    static bool relay(QObject *source, QObject *behind);

    /// Say that this Source holds the state every caller's view is made from, so its
    /// `\<scope\>` gated members are not gated on it.
    ///
    /// Only the one Source a shared entity answers everyone from. It is not any caller's
    /// view. It holds every value for all of them, and each mirror applies that caller's
    /// gate as it republishes. Every other Source answers exactly one caller and gates by
    /// default, which is the fail-closed way round. A Source nobody says this about hides
    /// a gated member rather than publishing it to whoever turns up.
    ///
    /// Returns false when the object does not answer it, which a contract with no gated
    /// member does not.
    static bool holdsSharedState(QObject *source);
};

} // namespace SynQt

#endif // SYNQT_SOURCEFACTORY_H
