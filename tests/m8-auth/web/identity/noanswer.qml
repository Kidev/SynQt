// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// A mapping hook that loads but answers nothing, with no scopeFor at all. A
// `return QStringLiteral("user")` fallback at the end of mapScope for this case would let a
// project whose hook was renamed or mistyped sign everybody in as an authenticated user.
IdentityMapping {
}
