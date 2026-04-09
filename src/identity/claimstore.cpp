// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "claimstore.h"

#include "constanttime.h"
#include "secrets.h"

namespace SynQt {

bool ClaimStore::hold(const QString &code, const QByteArray &sessionId,
                      const QString &challenge, qint64 nowMs)
{
    if (code.isEmpty() || sessionId.isEmpty() || m_claims.contains(code)) {
        return false;
    }
    Claim claim;
    claim.sessionId = sessionId;
    claim.challenge = challenge;
    claim.createdMs = nowMs;
    m_claims.insert(code, claim);
    return true;
}

QByteArray ClaimStore::take(const QString &code, const QString &verifier, qint64 nowMs,
                            qint64 ttlMs)
{
    if (code.isEmpty() || !m_claims.contains(code)) {
        return {};
    }

    // Removed before it is checked, so a wrong verifier spends the code: one code, one
    // attempt.
    const Claim claim{m_claims.take(code)};
    if (nowMs - claim.createdMs > ttlMs) {
        return {};
    }
    // The function the client used to derive what it registered (SynQt::challengeFor).
    if (!constantTimeEquals(challengeFor(verifier.toUtf8()), claim.challenge.toUtf8())) {
        return {};
    }
    return claim.sessionId;
}

void ClaimStore::expire(qint64 nowMs, qint64 ttlMs)
{
    for (auto it{m_claims.begin()}; it != m_claims.end();) {
        if (nowMs - it->createdMs > ttlMs) {
            it = m_claims.erase(it);
        } else {
            ++it;
        }
    }
}

int ClaimStore::count() const
{
    return static_cast<int>(m_claims.size());
}

} // namespace SynQt
