# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The Phoenix column's project. One BEAM node holding both the endpoint and the N subscriber
# processes, because the contract puts the publisher and every subscriber in one process so
# the interval is read on one clock (COLUMN-CONTRACT.md, "One process"). On the BEAM that is
# the natural way to write it as well as the required one.
#
# Versions are pinned rather than left to a range, so a newer Phoenix is a deliberate bump
# and not a surprise in a number.

defmodule SynqtBenchPhoenix.MixProject do
  use Mix.Project

  def project do
    [
      app: :synqt_bench_phoenix,
      version: "0.1.0",
      elixir: "~> 1.19",
      start_permanent: false,
      deps: deps()
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end

  defp deps do
    [
      {:phoenix, "1.8.13"},
      {:bandit, "1.12.5"},
      {:jason, "1.4.4"},
      # The subscriber side. Phoenix's own client is JavaScript, so an Elixir subscriber
      # speaks the channel protocol over a plain WebSocket client, which is what this is.
      {:websockex, "0.4.3"}
    ]
  end
end
