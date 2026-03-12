// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SESSIONSTATESOURCE_H
#define SYNQT_SESSIONSTATESOURCE_H

#include "rep_sessionstate_source.h"

#include <QObject>

namespace SynQt {

class Caller;

/// The web edge's own Source for the framework-supplied SessionState connect point: who
/// the visitor on this connection is, published to the client that is them.
///
/// WebEdge hosts one per accepted connection over that connection's own Caller, which
/// resolves the live session record and follows the credential rotation a scope change makes.
///
/// It publishes the scope this session was granted and the normalized identity the mapping
/// hook returned, nothing about anybody else. The session credential is never published: the
/// browser holds it in a cookie page script cannot read.
class SessionStateSource : public SessionStateSimpleSource
{
    Q_OBJECT

public:
    /// caller is this connection's own and must never be shared with another connection's
    /// Source. It is expected to outlive this object (WebEdge parents it here).
    explicit SessionStateSource(Caller *caller, QObject *parent = nullptr);

private:
    void publish();

    Caller *m_caller;
};

} // namespace SynQt

#endif // SYNQT_SESSIONSTATESOURCE_H
