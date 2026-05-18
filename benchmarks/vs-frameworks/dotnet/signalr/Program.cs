// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// .NET's column of the live-path comparison: an ASP.NET Core SignalR hub, with the N
// subscribers as SignalR clients in the same process. The same shape as
// node/live-bare.mjs, deliberately, so a reader can put the two files next to each other
// and see that the only difference is the runtime.
//
// SignalR rather than a bare WebSocket, because SignalR is what a .NET team reaches for
// when it wants "the server pushes and every client sees it". A bare System.Net.WebSockets
// column would be a second floor beside go-bare and rust-bare, and this table already has
// two.
//
// MessagePack rather than the default JSON protocol: each stack is measured at its best
// rather than at its default, the same allowance Socket.IO gets when it is given binary
// frames and no compression.
//
// The stamp is our own 8 bytes at the front of the payload, on Stopwatch's monotonic clock,
// and never SignalR's own timestamping: a column that measured a different interval would
// not be in the same table. See COLUMN-CONTRACT.md.

using System.Buffers.Binary;
using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.AspNetCore.SignalR;
using Microsoft.AspNetCore.SignalR.Client;

namespace SynQt.Bench.SignalR;

/// The hub. It carries no methods a client calls: in this workload subscribers never send,
/// they only receive, so all the hub does is count who is connected.
public sealed class LiveHub : Hub
{
    public override Task OnConnectedAsync()
    {
        Interlocked.Increment(ref Program.Connected);
        return base.OnConnectedAsync();
    }

    public override Task OnDisconnectedAsync(Exception? exception)
    {
        Interlocked.Decrement(ref Program.Connected);
        return base.OnDisconnectedAsync(exception);
    }
}

public static class Program
{
    public static int Connected;

    /// The process's monotonic origin. Every stamp is microseconds since this, on both ends,
    /// so what is reported is an interval.
    ///
    /// Stopwatch is the monotonic clock. DateTime.UtcNow is not, and using it here would put
    /// a clock step into the distribution as if it were a propagation time.
    private static readonly long Origin = Stopwatch.GetTimestamp();

    private static long NowMicros() =>
        (Stopwatch.GetTimestamp() - Origin) * 1_000_000 / Stopwatch.Frequency;

    /// Resident set size as the OS reports it, which is what every other column reports.
    /// The GC's own accounting (GC.GetTotalMemory) is a different measurement and does not
    /// go in this field.
    private static long ResidentBytes()
    {
        using var self = Process.GetCurrentProcess();
        self.Refresh();
        return self.WorkingSet64;
    }

    /// Process CPU in milliseconds, user plus system: the same figure every other column
    /// reports, and the one a host is sized on.
    private static double CpuMilliseconds()
    {
        using var self = Process.GetCurrentProcess();
        self.Refresh();
        return self.TotalProcessorTime.TotalMilliseconds;
    }

    /// Linear interpolation between ranks, exactly as measure.mjs does it. A nearest-rank
    /// percentile would disagree with every other column at small sample counts, which is
    /// the kind of difference that reads as a fact about the runtime.
    private static Distribution Summarize(List<double> samples)
    {
        var sorted = samples.ToArray();
        Array.Sort(sorted);
        double At(double fraction)
        {
            if (sorted.Length == 0)
            {
                return 0;
            }
            var rank = fraction * (sorted.Length - 1);
            var low = (int)Math.Floor(rank);
            var high = (int)Math.Ceiling(rank);
            if (low == high)
            {
                return sorted[low];
            }
            return sorted[low] + (rank - low) * (sorted[high] - sorted[low]);
        }
        return new Distribution
        {
            Unit = "ms",
            Samples = sorted.Length,
            Min = sorted.Length > 0 ? sorted[0] : 0,
            P50 = At(0.5),
            P95 = At(0.95),
            P99 = At(0.99),
            Max = sorted.Length > 0 ? sorted[^1] : 0,
            Mean = sorted.Length > 0 ? sorted.Average() : 0,
        };
    }

    /// `--flag value` and bare `--flag`, the same shape measure.mjs parses, so one set of
    /// arguments from run-bench.sh means the same thing to every column.
    private static Dictionary<string, string> ParseArgs(string[] argv)
    {
        var values = new Dictionary<string, string>
        {
            ["subscribers"] = "10,50,100,250",
            ["seconds"] = "5",
            ["hz"] = "30",
            ["payload"] = "256",
            ["saturate"] = "false",
            ["out"] = "",
        };
        for (var index = 0; index < argv.Length; index += 1)
        {
            if (!argv[index].StartsWith("--", StringComparison.Ordinal))
            {
                continue;
            }
            var name = argv[index][2..];
            var next = index + 1 < argv.Length ? argv[index + 1] : null;
            if (next is null || next.StartsWith("--", StringComparison.Ordinal))
            {
                values[name] = "true";
                continue;
            }
            values[name] = next;
            index += 1;
        }
        return values;
    }

    public static async Task<int> Main(string[] argv)
    {
        var args = ParseArgs(argv);
        var sizes = args["subscribers"].Split(',')
            .Select(part => int.TryParse(part.Trim(), out var size) ? size : 0)
            .Where(size => size > 0)
            .ToList();
        var seconds = Math.Max(1, int.Parse(args["seconds"]));
        var hz = Math.Max(1, int.Parse(args["hz"]));
        var payloadBytes = Math.Max(0, int.Parse(args["payload"]));
        var saturate = args["saturate"] == "true";

        Console.WriteLine(
            $"SignalR live path: {seconds}s at {(saturate ? "saturation" : $"{hz} Hz")}, " +
            $"{payloadBytes} byte payload, subscribers {args["subscribers"]}");

        var builder = WebApplication.CreateSlimBuilder();
        builder.Logging.ClearProviders();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Services.AddSignalR().AddMessagePackProtocol();
        var app = builder.Build();
        app.MapHub<LiveHub>("/live");
        await app.StartAsync();

        var addresses = app.Services.GetRequiredService<IServer>()
            .Features.Get<IServerAddressesFeature>();
        var url = addresses?.Addresses.First() ?? "http://127.0.0.1:5000";
        var hub = app.Services.GetRequiredService<IHubContext<LiveHub>>();

        var payload = new byte[payloadBytes];
        Array.Fill(payload, (byte)'x');
        var baselineRss = ResidentBytes();
        var sweep = new List<Entry>();

        foreach (var subscriberCount in sizes)
        {
            var measuring = false;
            var deliveredThisFrame = 0;
            var fleet = new SemaphoreSlim(0);
            // One sample list per subscriber rather than one shared behind a lock: N
            // handlers contending on one lock inside the receive path would be measuring the
            // lock, and the lock is the harness rather than the runtime.
            var samples = new List<double>[subscriberCount];
            var connections = new List<HubConnection>(subscriberCount);

            for (var index = 0; index < subscriberCount; index += 1)
            {
                samples[index] = new List<double>();
                var mine = samples[index];
                var connection = new HubConnectionBuilder()
                    .WithUrl($"{url}/live")
                    .AddMessagePackProtocol()
                    .Build();
                connection.On<byte[]>("frame", frame =>
                {
                    if (!Volatile.Read(ref measuring) || frame.Length < 8)
                    {
                        return;
                    }
                    var stamp = (long)BinaryPrimitives.ReadUInt64LittleEndian(frame);
                    mine.Add((NowMicros() - stamp) / 1000.0);
                    if (Interlocked.Increment(ref deliveredThisFrame) >= subscriberCount)
                    {
                        fleet.Release();
                    }
                });
                await connection.StartAsync();
                connections.Add(connection);
            }

            var deadline = DateTime.UtcNow.AddSeconds(30);
            while (Volatile.Read(ref Connected) < subscriberCount && DateTime.UtcNow < deadline)
            {
                await Task.Delay(10);
            }
            if (Volatile.Read(ref Connected) < subscriberCount)
            {
                Console.Error.WriteLine($"subscribers did not come up at N={subscriberCount}");
                return 1;
            }
            var connectedRss = ResidentBytes();

            byte[] Frame()
            {
                var frame = new byte[8 + payload.Length];
                BinaryPrimitives.WriteUInt64LittleEndian(frame, (ulong)NowMicros());
                payload.CopyTo(frame, 8);
                return frame;
            }

            // Warm up before measuring, for the same reason every other column does: the
            // first frames pay for a lazily built protocol pipeline on both ends and would
            // otherwise be the whole tail. `measuring` is still false, so none of it reaches
            // the statistics.
            var warmupTicks = Math.Min(hz, 30);
            for (var tick = 0; tick < warmupTicks; tick += 1)
            {
                await hub.Clients.All.SendAsync("frame", Frame());
                await Task.Delay(1000 / hz);
            }
            Volatile.Write(ref measuring, true);

            var ticks = 0;
            var cpuBefore = CpuMilliseconds();
            var window = Stopwatch.StartNew();
            if (saturate)
            {
                // Closed loop: publish, wait for the whole fleet to have it, publish again.
                // Open-looping at "maximum rate" would measure the send buffer rather than
                // the capacity, and the SynQt column cannot open-loop at all, so this is the
                // mode every column shares.
                while (window.Elapsed.TotalSeconds < seconds)
                {
                    Interlocked.Exchange(ref deliveredThisFrame, 0);
                    while (fleet.CurrentCount > 0)
                    {
                        await fleet.WaitAsync(0);
                    }
                    await hub.Clients.All.SendAsync("frame", Frame());
                    ticks += 1;
                    if (!await fleet.WaitAsync(TimeSpan.FromSeconds(5)))
                    {
                        Console.Error.WriteLine(
                            $"a frame never reached every subscriber at N={subscriberCount}");
                        return 1;
                    }
                }
            }
            else
            {
                ticks = seconds * hz;
                for (var tick = 0; tick < ticks; tick += 1)
                {
                    await hub.Clients.All.SendAsync("frame", Frame());
                    var due = TimeSpan.FromMilliseconds((tick + 1) * 1000.0 / hz);
                    var wait = due - window.Elapsed;
                    if (wait > TimeSpan.Zero)
                    {
                        await Task.Delay(wait);
                    }
                }
                // Let what is in flight land, or the tail of every run reads as loss that is
                // really the harness stopping first.
                await Task.Delay(500);
            }
            var elapsedSeconds = window.Elapsed.TotalSeconds;
            var cpuMs = CpuMilliseconds() - cpuBefore;
            Volatile.Write(ref measuring, false);

            var propagation = new List<double>();
            foreach (var one in samples)
            {
                propagation.AddRange(one);
            }
            var delivered = propagation.Count;
            var entry = new Entry
            {
                Subscribers = subscriberCount,
                Propagation = Summarize(propagation),
                ThroughputPerSec = delivered / Math.Max(elapsedSeconds, 0.001),
                CpuMsPer1k = delivered > 0 ? cpuMs * 1000 / delivered : 0,
                RssPerConn = subscriberCount > 0 && connectedRss > baselineRss
                    ? (double)(connectedRss - baselineRss) / subscriberCount
                    : 0,
                RssTotal = connectedRss,
                Delivered = delivered,
                Expected = ticks * subscriberCount,
            };
            Console.WriteLine(
                $"  N={entry.Subscribers}  p50 {entry.Propagation.P50:F3} ms" +
                $"  p99 {entry.Propagation.P99:F3} ms" +
                $"  {entry.ThroughputPerSec:F0} msg/s" +
                $"  delivered {entry.Delivered}/{entry.Expected}");
            sweep.Add(entry);

            foreach (var connection in connections)
            {
                await connection.DisposeAsync();
            }
            var closing = DateTime.UtcNow.AddSeconds(10);
            while (Volatile.Read(ref Connected) > 0 && DateTime.UtcNow < closing)
            {
                await Task.Delay(10);
            }
            await Task.Delay(100);
        }

        await app.StopAsync();

        var document = new Document
        {
            Benchmark = "vs-frameworks-live",
            Stack = "dotnet-signalr",
            Path = "ASP.NET Core SignalR, MessagePack over WebSockets",
            DotnetVersion = Environment.Version.ToString(),
            // "linux <kernel release>", which is what measure.mjs writes from os.platform()
            // and os.release(). The table's header prints one column's host for the whole
            // run, so two columns naming the machine differently would make it a lottery.
            Host = $"{(OperatingSystem.IsLinux() ? "linux" : "other")} " +
                   $"{Environment.OSVersion.Version}",
            Arch = System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture
                .ToString().ToLowerInvariant(),
            Recorded = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ"),
            RssAvailable = true,
            Hz = saturate ? 0 : hz,
            Saturated = saturate,
            Seconds = seconds,
            PayloadBytes = payloadBytes,
            Sweep = sweep,
        };
        var encoded = JsonSerializer.Serialize(document,
            new JsonSerializerOptions { WriteIndented = true });
        if (!string.IsNullOrEmpty(args["out"]))
        {
            await File.WriteAllTextAsync(args["out"], encoded + "\n");
            Console.WriteLine($"\nwrote {args["out"]}");
        }
        return 0;
    }
}

public sealed class Distribution
{
    [JsonPropertyName("unit")] public string Unit { get; set; } = "ms";
    [JsonPropertyName("samples")] public int Samples { get; set; }
    [JsonPropertyName("min")] public double Min { get; set; }
    [JsonPropertyName("p50")] public double P50 { get; set; }
    [JsonPropertyName("p95")] public double P95 { get; set; }
    [JsonPropertyName("p99")] public double P99 { get; set; }
    [JsonPropertyName("max")] public double Max { get; set; }
    [JsonPropertyName("mean")] public double Mean { get; set; }
}

public sealed class Entry
{
    [JsonPropertyName("subscribers")] public int Subscribers { get; set; }
    [JsonPropertyName("propagation")] public Distribution Propagation { get; set; } = new();
    [JsonPropertyName("throughput_msgs_per_sec")] public double ThroughputPerSec { get; set; }
    [JsonPropertyName("cpu_ms_per_1k")] public double CpuMsPer1k { get; set; }
    [JsonPropertyName("rss_bytes_per_conn")] public double RssPerConn { get; set; }
    [JsonPropertyName("rss_total_bytes")] public long RssTotal { get; set; }
    [JsonPropertyName("delivered")] public int Delivered { get; set; }
    [JsonPropertyName("expected")] public int Expected { get; set; }
}

public sealed class Document
{
    [JsonPropertyName("benchmark")] public string Benchmark { get; set; } = "";
    [JsonPropertyName("stack")] public string Stack { get; set; } = "";
    [JsonPropertyName("path")] public string Path { get; set; } = "";
    [JsonPropertyName("dotnet_version")] public string DotnetVersion { get; set; } = "";
    [JsonPropertyName("host")] public string Host { get; set; } = "";
    [JsonPropertyName("arch")] public string Arch { get; set; } = "";
    [JsonPropertyName("recorded")] public string Recorded { get; set; } = "";
    [JsonPropertyName("rss_available")] public bool RssAvailable { get; set; }
    [JsonPropertyName("hz")] public int Hz { get; set; }
    [JsonPropertyName("saturated")] public bool Saturated { get; set; }
    [JsonPropertyName("seconds")] public int Seconds { get; set; }
    [JsonPropertyName("payload_bytes")] public int PayloadBytes { get; set; }
    [JsonPropertyName("sweep")] public List<Entry> Sweep { get; set; } = new();
}
