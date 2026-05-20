# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Elixir's column of the live-path comparison: a Phoenix endpoint with one channel, and the
# N subscribers as real WebSocket clients on the same BEAM node. The same shape as
# node/live-bare.mjs, deliberately, so a reader can put the two files next to each other and
# see that the only difference is the runtime.
#
# Phoenix earns this column by being the stack SynQt is most often said to be like: a server
# that holds live state and pushes it. Channels over one node running both the endpoint and
# the subscribers honours the one-process rule exactly, and idiomatically.
#
# The subscribers are WebSocket clients and not bare `Phoenix.PubSub.subscribe/2` processes,
# and that is the whole difference between a fair column and a flattering one: subscribing to
# the PubSub topic in-node would skip the channel stack, the serializer and the socket, which
# is precisely the transport every other column is measured carrying.
#
# `System.monotonic_time/1` and never `System.system_time/1`: the same clock reads the stamp
# back, so what is measured is an interval. See COLUMN-CONTRACT.md.

defmodule Live.Channel do
  @moduledoc "The channel every subscriber joins. Nothing is sent to it; it only receives."
  use Phoenix.Channel

  def join("live", _payload, socket), do: {:ok, socket}
end

defmodule Live.Socket do
  @moduledoc false
  use Phoenix.Socket

  channel("live", Live.Channel)

  def connect(_params, socket, _connect_info), do: {:ok, socket}
  def id(_socket), do: nil
end

defmodule Live.Endpoint do
  @moduledoc false
  use Phoenix.Endpoint, otp_app: :synqt_bench_phoenix

  socket("/socket", Live.Socket, websocket: true, longpoll: false)
end

defmodule Live.Subscriber do
  @moduledoc """
  One subscriber: a WebSocket client that joins the channel and records the interval of
  every broadcast it receives.

  Deliberately the shortest receive path the protocol allows. Anything slow here would be a
  cost the harness inflicted rather than one the stack has.
  """
  use WebSockex

  def start_link(url, index, owner) do
    # `start` and not `start_link`: the sweep kills every subscriber when a size is done,
    # and a linked subscriber would take the process running the sweep with it.
    with {:ok, pid} <-
           WebSockex.start(url, __MODULE__,
             %{index: index, owner: owner, samples: [], measuring: false},
             async: false
           ) do
      # The join goes out here rather than from handle_connect/2, which WebSockex does not
      # let a callback reply from: it takes {:ok, state} only.
      #
      # The v2 serializer's envelope is a five-element array, not an object:
      # [join_ref, ref, topic, event, payload]. Sending the object form connects, joins
      # nothing, and leaves the sweep waiting forever for a confirmation that was never
      # asked for.
      :ok =
        WebSockex.send_frame(
          pid,
          {:text, Jason.encode!(["1", "1", "live", "phx_join", %{}])}
        )

      {:ok, pid}
    end
  end

  def handle_connect(_conn, state), do: {:ok, state}

  def handle_frame({:text, body}, state) do
    case Jason.decode(body) do
      {:ok, [_join_ref, _ref, "live", "phx_reply", %{"status" => "ok"}]} ->
        send(state.owner, {:joined, state.index})
        {:ok, state}

      {:ok, [_join_ref, _ref, "live", "frame", %{"b" => encoded}]} ->
        if state.measuring do
          # Phoenix's channel envelope is JSON, so the 8-byte stamp travels base64 inside
          # it. The frame layout is the contract's; the encoding around it is what Phoenix
          # actually puts on the wire, and the README says so.
          <<stamp::little-unsigned-64, _rest::binary>> = Base.decode64!(encoded)
          now = Live.Run.now_micros()
          send(state.owner, :delivered)
          {:ok, %{state | samples: [(now - stamp) / 1000 | state.samples]}}
        else
          {:ok, state}
        end

      _ ->
        {:ok, state}
    end
  end

  def handle_frame(_frame, state), do: {:ok, state}

  def handle_cast({:measuring, value}, state), do: {:ok, %{state | measuring: value}}
  def handle_cast(:report, state) do
    send(state.owner, {:samples, state.index, state.samples})
    {:ok, %{state | samples: []}}
  end
end

defmodule Live.Run do
  @moduledoc "The sweep, and the JSON it writes."

  require Logger

  @origin_key {__MODULE__, :origin}

  def now_micros do
    origin = :persistent_term.get(@origin_key)
    System.monotonic_time(:microsecond) - origin
  end

  @doc """
  Resident set size as the OS reports it, which is what every other column reports.

  `:erlang.memory(:total)` is the BEAM's own accounting of what it has allocated, which is a
  different measurement: it excludes the code the VM mapped and includes memory the
  allocators are holding but not using. Putting it in this field would be a different number
  in the same cell. The README carries it as a note instead.
  """
  def resident_bytes do
    case File.read("/proc/self/statm") do
      {:ok, text} ->
        text |> String.split() |> Enum.at(1) |> String.to_integer() |> Kernel.*(4096)

      _ ->
        0
    end
  end

  @doc """
  Process CPU in milliseconds, user plus system: the figure a host is sized on, and the one
  every other column reports.

  Read from `/proc/self/stat` rather than from the BEAM. `:erlang.statistics(:runtime)` is
  scheduler run time, which on a 32-scheduler node is a different quantity from "CPU this OS
  process burned" and would not be comparable with any other column, and `:cpu_sup` reports
  the whole machine.
  """
  def cpu_milliseconds do
    case File.read("/proc/self/stat") do
      {:ok, text} ->
        # utime and stime, fields 14 and 15, in clock ticks. Everything before them can
        # contain spaces inside parentheses (the comm field), so the split starts after the
        # closing bracket rather than at the beginning of the line.
        [_, rest] = String.split(text, ") ", parts: 2)
        fields = String.split(rest)

        ticks =
          String.to_integer(Enum.at(fields, 11)) + String.to_integer(Enum.at(fields, 12))

        ticks * 1000 / clock_ticks_per_second()

      _ ->
        0.0
    end
  end

  # sysconf(_SC_CLK_TCK). The BEAM exposes no way to ask, and it has been 100 on every Linux
  # this harness targets; a machine where it is not would make this column's CPU row wrong by
  # that ratio and nothing else.
  defp clock_ticks_per_second, do: 100

  @doc """
  Linear interpolation between ranks, exactly as measure.mjs does it. A nearest-rank
  percentile would disagree with every other column at small sample counts.
  """
  def summarize(samples) do
    sorted = Enum.sort(samples)
    count = length(sorted)
    indexed = List.to_tuple(sorted)

    at = fn fraction ->
      if count == 0 do
        0.0
      else
        rank = fraction * (count - 1)
        low = trunc(rank)
        high = min(low + 1, count - 1)

        if low == high do
          elem(indexed, low)
        else
          elem(indexed, low) + (rank - low) * (elem(indexed, high) - elem(indexed, low))
        end
      end
    end

    %{
      "unit" => "ms",
      "samples" => count,
      "min" => if(count > 0, do: elem(indexed, 0), else: 0.0),
      "p50" => at.(0.5),
      "p95" => at.(0.95),
      "p99" => at.(0.99),
      "max" => if(count > 0, do: elem(indexed, count - 1), else: 0.0),
      "mean" => if(count > 0, do: Enum.sum(sorted) / count, else: 0.0)
    }
  end

  @doc """
  `--flag value` and bare `--flag`, the same shape measure.mjs parses, so one set of
  arguments from run-bench.sh means the same thing to every column.
  """
  def parse_args(argv, defaults) do
    Enum.reduce(Enum.with_index(argv), defaults, fn {token, index}, acc ->
      case token do
        "--" <> name ->
          value = Enum.at(argv, index + 1)

          if is_nil(value) or String.starts_with?(value, "--") do
            Map.put(acc, name, "true")
          else
            Map.put(acc, name, value)
          end

        _ ->
          acc
      end
    end)
  end

  def main(argv) do
    :persistent_term.put(@origin_key, System.monotonic_time(:microsecond))

    args =
      parse_args(argv, %{
        "subscribers" => "10,50,100,250",
        "seconds" => "5",
        "hz" => "30",
        "payload" => "256",
        "saturate" => "false",
        "out" => ""
      })

    sizes =
      args["subscribers"]
      |> String.split(",")
      |> Enum.map(&(&1 |> String.trim() |> Integer.parse()))
      |> Enum.flat_map(fn
        {size, _} when size > 0 -> [size]
        _ -> []
      end)

    seconds = max(1, String.to_integer(args["seconds"]))
    rate = max(1, String.to_integer(args["hz"]))
    payload_bytes = max(0, String.to_integer(args["payload"]))
    saturate = args["saturate"] == "true"

    IO.puts(
      "Phoenix Channels live path: #{seconds}s at " <>
        "#{if saturate, do: "saturation", else: "#{rate} Hz"}, " <>
        "#{payload_bytes} byte payload, subscribers #{args["subscribers"]}"
    )

    # The endpoint logs a line per connection and per join, which at N=250 is a thousand
    # lines of noise around three lines of measurement.
    Logger.configure(level: :warning)

    {:ok, _} = Application.ensure_all_started(:phoenix)
    {:ok, _} = Application.ensure_all_started(:websockex)

    Application.put_env(:synqt_bench_phoenix, Live.Endpoint,
      http: [ip: {127, 0, 0, 1}, port: 0],
      server: true,
      secret_key_base: String.duplicate("benchmark-only-never-a-deployment", 2),
      pubsub_server: Live.PubSub,
      render_errors: [formats: []],
      adapter: Bandit.PhoenixAdapter
    )

    {:ok, _} =
      Supervisor.start_link(
        [{Phoenix.PubSub, name: Live.PubSub}, Live.Endpoint],
        strategy: :one_for_one
      )

    # Bound with port 0, so the port is whatever the OS gave it and has to be read back.
    # Bandit runs on ThousandIsland rather than ranch, and `server_info/1` is the
    # adapter-independent way Phoenix answers this.
    {:ok, {_ip, port}} = Live.Endpoint.server_info(:http)
    url = "ws://127.0.0.1:#{port}/socket/websocket?vsn=2.0.0"

    payload = String.duplicate("x", payload_bytes)
    baseline_rss = resident_bytes()

    sweep =
      Enum.map(sizes, fn subscriber_count ->
        sweep_one(url, subscriber_count, payload, seconds, rate, saturate, baseline_rss)
      end)

    document = %{
      "benchmark" => "vs-frameworks-live",
      "stack" => "phoenix",
      "path" => "Phoenix Channels on Bandit, JSON envelope over WebSockets",
      "elixir_version" => System.version(),
      "otp_release" => List.to_string(:erlang.system_info(:otp_release)),
      "beam_memory_total_bytes" => :erlang.memory(:total),
      "host" => "linux #{File.read!("/proc/sys/kernel/osrelease") |> String.trim()}",
      "arch" => :erlang.system_info(:system_architecture) |> List.to_string(),
      "recorded" => DateTime.utc_now() |> DateTime.to_iso8601(),
      "rss_available" => true,
      "hz" => if(saturate, do: 0, else: rate),
      "saturated" => saturate,
      "seconds" => seconds,
      "payload_bytes" => payload_bytes,
      "sweep" => sweep
    }

    if args["out"] != "" do
      File.write!(args["out"], Jason.encode!(document, pretty: true) <> "\n")
      IO.puts("\nwrote #{args["out"]}")
    end
  end

  defp sweep_one(url, subscriber_count, payload, seconds, rate, saturate, baseline_rss) do
    owner = self()

    subscribers =
      Enum.map(0..(subscriber_count - 1), fn index ->
        {:ok, pid} = Live.Subscriber.start_link(url, index, owner)
        pid
      end)

    await_joins(subscriber_count)
    connected_rss = resident_bytes()

    publish = fn ->
      frame = <<now_micros()::little-unsigned-64>> <> payload
      Live.Endpoint.broadcast!("live", "frame", %{"b" => Base.encode64(frame)})
    end

    # Warm up before measuring, for the same reason every other column does: the first
    # frames pay for lazily built buffers on both ends and would otherwise be the whole tail.
    # The subscribers are not measuring yet, so none of it reaches the statistics.
    Enum.each(1..min(rate, 30), fn _ ->
      publish.()
      Process.sleep(div(1000, rate))
    end)

    Enum.each(subscribers, &WebSockex.cast(&1, {:measuring, true}))
    drain_mailbox()

    cpu_before = cpu_milliseconds()
    started_at = System.monotonic_time(:millisecond)

    ticks =
      if saturate do
        saturating(publish, subscriber_count, started_at, seconds, 0)
      else
        total = seconds * rate

        Enum.each(0..(total - 1), fn tick ->
          publish.()
          due = started_at + div((tick + 1) * 1000, rate)
          wait = due - System.monotonic_time(:millisecond)
          if wait > 0, do: Process.sleep(wait)
        end)

        # Let what is in flight land, or the tail of every run reads as loss that is really
        # the harness stopping first.
        Process.sleep(500)
        total
      end

    elapsed_seconds = (System.monotonic_time(:millisecond) - started_at) / 1000
    cpu_ms = cpu_milliseconds() - cpu_before
    Enum.each(subscribers, &WebSockex.cast(&1, {:measuring, false}))

    Enum.each(subscribers, &WebSockex.cast(&1, :report))
    propagation = collect_samples(subscriber_count, [])
    delivered = length(propagation)

    Enum.each(subscribers, fn pid -> Process.exit(pid, :kill) end)
    Process.sleep(200)

    entry = %{
      "subscribers" => subscriber_count,
      "propagation" => summarize(propagation),
      "throughput_msgs_per_sec" => delivered / max(elapsed_seconds, 0.001),
      "cpu_ms_per_1k" => if(delivered > 0, do: cpu_ms * 1000 / delivered, else: 0.0),
      "rss_bytes_per_conn" =>
        if(subscriber_count > 0 and connected_rss > baseline_rss,
          do: (connected_rss - baseline_rss) / subscriber_count,
          else: 0.0
        ),
      "rss_total_bytes" => connected_rss,
      "delivered" => delivered,
      "expected" => ticks * subscriber_count
    }

    spread = entry["propagation"]

    IO.puts(
      "  N=#{subscriber_count}" <>
        "  p50 #{:erlang.float_to_binary(spread["p50"] / 1, decimals: 3)} ms" <>
        "  p99 #{:erlang.float_to_binary(spread["p99"] / 1, decimals: 3)} ms" <>
        "  #{round(entry["throughput_msgs_per_sec"])} msg/s" <>
        "  delivered #{delivered}/#{entry["expected"]}"
    )

    entry
  end

  defp saturating(publish, subscriber_count, started_at, seconds, ticks) do
    if System.monotonic_time(:millisecond) - started_at >= seconds * 1000 do
      ticks
    else
      # Closed loop: publish, wait for the whole fleet to have it, publish again.
      # Open-looping at "maximum rate" would measure the send buffer rather than the
      # capacity, and the SynQt column cannot open-loop at all, so this is the mode every
      # column shares.
      publish.()
      await_fleet(subscriber_count)
      saturating(publish, subscriber_count, started_at, seconds, ticks + 1)
    end
  end

  defp await_fleet(0), do: :ok

  defp await_fleet(remaining) do
    receive do
      :delivered -> await_fleet(remaining - 1)
    after
      5000 ->
        IO.puts(:stderr, "a frame never reached every subscriber")
        System.halt(1)
    end
  end

  defp await_joins(0), do: :ok

  defp await_joins(remaining) do
    receive do
      {:joined, _index} -> await_joins(remaining - 1)
    after
      30_000 ->
        IO.puts(:stderr, "subscribers did not come up")
        System.halt(1)
    end
  end

  defp collect_samples(0, acc), do: acc

  defp collect_samples(remaining, acc) do
    receive do
      {:samples, _index, samples} -> collect_samples(remaining - 1, samples ++ acc)
    after
      30_000 -> acc
    end
  end

  defp drain_mailbox do
    receive do
      _ -> drain_mailbox()
    after
      0 -> :ok
    end
  end
end
