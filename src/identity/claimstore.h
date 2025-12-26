// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CLAIMSTORE_H
#define SYNQT_CLAIMSTORE_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QtGlobal>

namespace SynQt {

/// Finished desktop logins waiting to be collected.
///
/// A desktop sign-in ends at a loopback redirect carrying a one-time code, not a cookie:
/// the system browser is not the app, and leaving a live session in it would put one on a
/// machine that may not be the visitor's alone with nothing to ever end it. What crosses
/// the loopback is this code, which stands for the session for about a minute and is
/// exchangeable once, by whoever holds the verifier for the challenge it was minted with.
///
/// It is a class rather than a hash inside the edge because the code is minted on whichever
/// process answered the callback and redeemed over a connection the client opens fresh,
/// which a balancer places independently. With one edge those are the same process. With
/// several they are not, and a table in one process's memory is a claim the other processes
/// cannot honour. So the edge keeps one of these only when identity runs in process, and
/// asks the auth entity otherwise, and both paths run this same code.
class ClaimStore
{
public:
    struct Claim
    {
        QByteArray sessionId;
        QString challenge;  ///< base64url S256; the verifier is what the client presents
        qint64 createdMs{0};
    };

    /// Hold a claim. False when the code is already in use, which a caller minting random
    /// codes never sees and which must never silently replace the claim already there.
    bool hold(const QString &code, const QByteArray &sessionId, const QString &challenge,
              qint64 nowMs);

    /// Spend a claim, if the verifier matches its challenge and it has not expired.
    ///
    /// The code is taken out before it is checked, deliberately: a wrong verifier spends it
    /// rather than leaving it there to be tried again. One code, one attempt.
    ///
    /// An empty return is every failure alike (unknown, expired, already spent, wrong
    /// verifier), because telling them apart tells a guesser which half of a guess was
    /// right.
    QByteArray take(const QString &code, const QString &verifier, qint64 nowMs, qint64 ttlMs);

    /// Drop claims past their time to live. Swept on the way in to the routes that add one,
    /// so an uncollected code cannot outlive its minute even on a process nobody signs into
    /// again. A claim that expires takes nothing with it. The session it stood for is a real
    /// session, and it lives or expires on the session manager's own terms.
    void expire(qint64 nowMs, qint64 ttlMs);

    int count() const;

private:
    QHash<QString, Claim> m_claims;
};

/// How long a desktop claim code may stand for its session, from the configured seconds.
///
/// This is a machine-to-machine hop that happens the instant the loopback listener is hit,
/// not a human step, so it is short. The code has already been written into the
/// system browser's history by the time it exists, and its whole defence is being useless
/// by the time anyone reads it back. Clamped rather than trusted, because the configured
/// value can only make it worse. One definition, because the edge and the auth entity have
/// to agree on it exactly or a claim expires on one and not the other.
inline qint64 claimTtlMsFrom(int configuredSeconds)
{
    return 1000 * qBound(1, configuredSeconds, 300);
}

} // namespace SynQt

#endif // SYNQT_CLAIMSTORE_H
