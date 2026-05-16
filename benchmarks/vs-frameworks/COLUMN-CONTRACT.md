<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The column contract

This file is what every column in `benchmarks/vs-frameworks/` is held to, and what you read
before adding one. It exists because a table is only a comparison if every cell in it was
produced the same way. A column measured differently is a different number wearing the
table's formatting.

The reference implementations are [`node/measure.mjs`](node/measure.mjs) (the shared
measurement half of the Node columns) and [`bench_live.cpp`](bench_live.cpp) (the SynQt and
`qt-raw` columns). Where this file and those disagree, they are the truth and this file is
the bug. Read one of them before writing a column. This is the summary rather than the source.

## The workload

One publisher changes one value at a fixed rate. N subscribers must each see every
change. That is the whole benchmark, and it is the workload SynQt exists for, which is why
it is the headline rather than a request-per-second figure.

The sweep runs the whole thing once per subscriber count. Defaults, identical on every
column:

| Knob | Default | Flag |
| --- | --- | --- |
| Subscriber counts | `10,50,100,250` | `--subscribers` |
| Measured window per size | 5 seconds | `--seconds` |
| Publish rate | 30 Hz | `--hz` |
| Payload | 256 bytes | `--payload` |
| Saturation mode | off | `--saturate` |

A column parses those four flags with those defaults, because `run-bench.sh` forwards one
set of arguments to every column and a column that took a different default would sweep a
different workload under the same heading.

## The frame

The frame is 8 bytes of little-endian unsigned microseconds, then the payload. The stamp is read off
the process's monotonic clock at the moment the publisher sends, and read back off the same
clock in the subscriber's handler. What is reported is the difference, so it is an interval
and never a difference between two clocks.

Per runtime, the monotonic clock is:

| Runtime | Use | Never |
| --- | --- | --- |
| C++/Qt | `QElapsedTimer`, `std::chrono::steady_clock` | `QDateTime::currentMSecsSinceEpoch` |
| Node | `process.hrtime.bigint()` | `Date.now()` |
| Go | a difference of two `time.Time` values (they carry a monotonic reading) | `time.Now().UnixMicro()` differences |
| Rust | `std::time::Instant` | `SystemTime` |
| .NET | `Stopwatch.GetTimestamp()` | `DateTime.UtcNow` |
| Elixir | `System.monotonic_time/1` | `System.system_time/1` |
| Python | `time.perf_counter_ns()` | `time.time()` |
| Ruby | `Process.clock_gettime(Process::CLOCK_MONOTONIC)` | `Time.now` |
| PHP | `hrtime(true)` | `microtime()` |

Do not use the stack's own timestamping (SignalR's, Phoenix's, Socket.IO's). It measures a
different interval, and a column that measured a different interval is not in this table.

## One process

The publisher and every subscriber live in one OS process, so both ends of the interval
read the same clock and no part of the number is clock skew.

Two columns cannot honour this, and both say so where their number is reported:

- **`node-nextjs`.** Next.js has no WebSocket server, so its live path is a Route Handler
  streaming server-sent events. Still one process. The caveat is the protocol rather than the
  clock.
- **`php-reverb`.** Reverb is a standalone ReactPHP server, so the publisher goes through
  Laravel's broadcast path into a second process. Both ends are still under one OS clock, so
  the interval is still an interval, but it includes an inter-process hop no other column
  pays.

A column that cannot honour a rule here does not get a quiet exemption. It gets a sentence,
in the README, in the same paragraph as its number.

## The window

Per subscriber count, in order:

1. **Connect** every subscriber and wait until the server sees all N. Read RSS here; the
   difference from the process baseline, over N, is `rss_bytes_per_conn`.
2. **Warm up**: `min(hz, 30)` publishes at the publish rate, discarded. The first frames pay
   for lazily built buffers on both ends and would otherwise be the whole tail.
3. **Reset**: clear the samples and the per-subscriber delivered counts. The warm-up must
   not reach the statistics.
4. **Measure** for `seconds`, publishing at `hz`. In saturation mode instead: publish, wait
   for the whole fleet to have that frame, publish again. Open-looping at "maximum rate"
   measures the send buffer rather than the capacity, and QtRO coalesces outbound property
   changes, so the SynQt column cannot open-loop at all.
5. **Drain**: 500 ms in fixed-rate mode, so frames still in flight land inside the window.
   Without it the tail of every run reads as loss that is really the harness stopping first.
   Saturation mode needs no drain: it already waits for each frame.
6. **Tear down** every subscriber and the server before the next size, so a size is not
   measured with the previous one's sockets still open.

`expected` is `ticks * subscribers`. **`delivered` must equal `expected`.** A column that
dropped frames is not a faster column, it is a broken one, and every other number in it is a
figure over the survivors.

## The output

One JSON file per column, these keys, exactly as `measure.mjs` writes them:

```json
{
  "benchmark": "vs-frameworks-live",
  "stack": "go-bare",
  "path": "net/http + coder/websocket",
  "host": "linux 6.17.9",
  "arch": "x64",
  "recorded": "2026-09-07T10:11:12.000Z",
  "rss_available": true,
  "hz": 30,
  "saturated": false,
  "seconds": 5,
  "payload_bytes": 256,
  "sweep": [
    {
      "subscribers": 10,
      "propagation": {
        "unit": "ms", "samples": 1500,
        "min": 0.041, "p50": 0.088, "p95": 0.163, "p99": 0.241,
        "max": 1.902, "mean": 0.097
      },
      "throughput_msgs_per_sec": 299.7,
      "cpu_ms_per_1k": 41.2,
      "rss_bytes_per_conn": 74112,
      "rss_total_bytes": 41287680,
      "delivered": 1500,
      "expected": 1500
    }
  ]
}
```

- `benchmark` is `vs-frameworks-live`. `benchmarks/baselines.py` dispatches on it.
- `stack` is the column's name and is what `compare.py`'s `STACK_ORDER` registers.
- `path` is one short phrase naming what is on the wire.
- Percentiles are linear interpolation between ranks over the sorted samples, as
  `measure.mjs:distribution` does it. Do not use a nearest-rank percentile, because it disagrees
  with every other column at small sample counts.
- `cpu_ms_per_1k` is process CPU (user plus system) over the window, per thousand
  deliveries. It is what a host is sized on, so it must be the process's own CPU and not a
  wall-clock figure.
- `rss_total_bytes` is RSS as the OS reports it, for every column. A runtime with its
  own accounting (the BEAM's `:erlang.memory/1`, the JVM's heap) reports that in the README
  as a note, never in this field, because two different measurements in one cell is not a column.

The call table is a separate shape (`callers`, `latency`, `completed`, `failed`). See
`summarizeCalls` in `measure.mjs`. It answers a different question and shares only its
statistics.

## Adding a column

1. Write the program under `benchmarks/vs-frameworks/<runtime>/`, with its dependency
   manifest beside it. A column's server, its clients and its manifest are one directory.
2. Mirror the structure of `node/live-bare.mjs` and say at the top of the file that you did,
   so a reader can put the two side by side and see that the only difference is the runtime.
3. Register the stack name in `compare.py`'s `STACK_ORDER`.
4. Add a run block to `run-bench.sh`, guarded by `have <tool>` so a missing toolchain
   **skips with a printed reason** and never fails the run. A table missing a row a reader
   can see was skipped is more useful than no table.
5. Pin the toolchain in the manifest, so a newer one is a deliberate bump.
6. Add a row to the README's columns table saying what the column is, and, if it bends
   anything above, what it is not.
7. Check the shape, not the exit code: every sweep entry must have
   `delivered == expected` and `distribution.samples > 0`. A new column that is
   suspiciously fast and dropped frames is the failure mode to look for.

**Every column is printed, including one that loses.** A comparison that prints only its
winner is an advertisement.
