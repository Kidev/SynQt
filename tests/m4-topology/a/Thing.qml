// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The authoritative Source for the "thing" connect point, owned by entity A. The
// runtime instantiates this from QML and enables remoting on it. It sets the push
// property the consumer reads.
import SynQt

ThingSource {
    value: 42
}
