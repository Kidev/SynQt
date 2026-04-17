// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_RATEWINDOW_H
#define SYNQT_RATEWINDOW_H

#include <QHash>
#include <QString>

namespace SynQt {

/// One caller's budget inside the current window. When it opened, and what has been spent.
///
/// Fixed-window counting, which is what every gate in this framework needs and none of them
/// needs more than. The question is only ever "has this address made a nuisance of itself in
/// the last minute", and a sliding window would cost per-request bookkeeping to answer it
/// more precisely than anybody asks.
struct RateWindow
{
    qint64 startedMs{0};
    int count{0};
};

/// Drop the windows that have run out, and report whether the table is still at or above
/// `cap`.
///
/// Emptying the table when it grows would let anyone with many addresses (an IPv6 /64, a
/// trusted forwarding header) reset every count. Only expired windows are dropped; when that
/// is not enough, the caller refuses.
inline bool pruneRateWindows(QHash<QString, RateWindow> &windows, qint64 now,
                             qint64 windowMs, int cap)
{
    if (windows.size() < cap) {
        return false;
    }
    for (auto it{windows.begin()}; it != windows.end();) {
        if (now - it->startedMs > windowMs) {
            it = windows.erase(it);
        } else {
            ++it;
        }
    }
    return windows.size() >= cap;
}

/// How long a refused caller is told to wait, in whole seconds: rounded up and never zero, so
/// a client never retries inside its window and never reads `Retry-After: 0`.
inline qint64 retryAfterSeconds(qint64 remainingMs)
{
    return qMax(qint64{1}, (remainingMs + 999) / 1000);
}

} // namespace SynQt

#endif // SYNQT_RATEWINDOW_H
