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

/// Drop the windows that have run out, and report whether the table is still over its cap.
///
/// This exists because the obvious way to bound a table keyed by caller address is to empty
/// it when it grows too large, and that turns the cap into a reset anybody can pull. Every
/// gate here rations by address, so an attacker who can present addresses -- an IPv6 /64 is
/// a practically unlimited supply of them, and so is a forwarding header on a deployment
/// that trusts one -- fills the table with entries they will never use again, the table is
/// emptied, and the count against the address they are actually guessing from goes back to
/// zero. Repeat, and the gate is gone.
///
/// A window that has run out is not evidence of anything and dropping it costs nothing, so
/// that is what is dropped. The caller decides what to do when pruning was not enough,
/// which is the case where the addresses really are live and the answer is to refuse rather
/// than to forget.
///
/// Returns true when the table is still at or above `cap` after the prune.
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

} // namespace SynQt

#endif // SYNQT_RATEWINDOW_H
