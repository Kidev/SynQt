// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CONSTANTTIME_H
#define SYNQT_CONSTANTTIME_H

#include <QByteArray>
#include <QString>

namespace SynQt {

/// Compare two secrets without leaking where they first differ.
///
/// An ordinary comparison returns at the first differing byte, so how long it took says how
/// much of a guess was right, and a guesser who can measure that recovers a secret byte by
/// byte instead of all at once. This one always reads the whole of the shorter side and
/// folds every difference into one accumulator, so the time it takes is a fact about the
/// length and nothing else. Unequal lengths are refused up front, which leaks only the
/// length, and every secret compared through here is fixed-length anyway.
///
/// An empty left side is false rather than "equal to another empty one": every caller here
/// is checking something presented against something stored, and "nothing was presented"
/// must never be a match.
inline bool constantTimeEquals(const QByteArray &lhs, const QByteArray &rhs)
{
    if (lhs.isEmpty() || lhs.size() != rhs.size()) {
        return false;
    }
    quint8 difference{0};
    for (qsizetype i{0}; i < lhs.size(); ++i) {
        difference |= static_cast<quint8>(lhs.at(i)) ^ static_cast<quint8>(rhs.at(i));
    }
    return difference == 0;
}

inline bool constantTimeEquals(const QString &lhs, const QString &rhs)
{
    return constantTimeEquals(lhs.toUtf8(), rhs.toUtf8());
}

} // namespace SynQt

#endif // SYNQT_CONSTANTTIME_H
