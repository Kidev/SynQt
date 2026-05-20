# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The entry point, so the column runs the same way every other one does: one command with
# the shared flags. `mix run run.exs -- --subscribers ... --seconds ...`.
Live.Run.main(System.argv())
