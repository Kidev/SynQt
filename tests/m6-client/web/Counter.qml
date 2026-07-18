// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The authoritative counter Source on the web edge. The edge is the only writer.
import SynQt

CounterSource {
    value: 0

    function increment() { value = value + 1 }
    function decrement() { value = value - 1 }
}
