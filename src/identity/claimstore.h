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
/// A desktop sign-in ends at a loopback redirect carrying a one-time code, never a session:
/// the code stands for the session for about a minute and is exchangeable once, by whoever
/// holds the verifier for its challenge. A class, because with several edges the code may be
/// minted and redeemed on different processes: the edge keeps one only when identity runs in
/// process, and asks the auth entity otherwise.
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

    /// Spend a claim, if the verifier matches its challenge and it has not expired. The code is
    /// removed before the check, so a wrong verifier spends it. Every failure returns the same
    /// empty answer.
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

/// How long a desktop claim code may stand for its session, from the configured seconds,
/// clamped. The code is already in the system browser's history, so it must expire fast. One
/// definition, so the edge and the auth entity agree.
inline qint64 claimTtlMsFrom(int configuredSeconds)
{
    return 1000 * qBound(1, configuredSeconds, 300);
}

} // namespace SynQt

#endif // SYNQT_CLAIMSTORE_H
