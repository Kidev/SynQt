<?php
// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// PHP's column of the live-path comparison: Laravel Reverb, with the N subscribers as
// ratchet/pawl WebSocket clients on one ReactPHP loop. The same shape as
// node/live-bare.mjs, so a reader can put the two files next to each other
// and see that the only difference is the runtime.
//
// This column pays a hop no other one does, and the README says so where the number is
// reported. Reverb is a standalone ReactPHP server, so a deployment is: the application
// broadcasts (a signed HTTP call), Reverb fans out, the browser receives. Three processes,
// and this harness runs all three. What that costs the measurement is a process boundary
// inside the interval. What it buys is a number about the thing people deploy.
//
// The clock is still one clock. hrtime(true) is CLOCK_MONOTONIC on Linux, whose origin is
// the boot and not the process, so the stamp written in the publisher is read back here
// against the same zero. See COLUMN-CONTRACT.md.
//
// The subscribers are fibers-in-spirit: one event loop, N connections, no threads. PHP has
// no threads to get this wrong with, which is the one place this column has an easier job
// than the Ruby one.

require __DIR__.'/vendor/autoload.php';

use Ratchet\Client\Connector;
use React\EventLoop\Loop;

/// Microseconds on the monotonic clock. hrtime(true) is nanoseconds since an unspecified
/// origin that is the boot on Linux, and never microtime(), which is a wall clock and would
/// put a clock step into the distribution as if it were a propagation time.
function now_micros(): int
{
    return intdiv(hrtime(true), 1000);
}

/// Resident set size as the OS reports it, which is what every other column reports.
/// memory_get_usage() is PHP's own arena and is a different measurement.
function resident_bytes(): int
{
    $statm = @file_get_contents('/proc/self/statm');
    if ($statm === false) {
        return 0;
    }
    $fields = preg_split('/\s+/', trim($statm));

    return isset($fields[1]) ? ((int) $fields[1]) * 4096 : 0;
}

/// Process CPU in milliseconds, user plus system. The figure a host is sized on.
function cpu_milliseconds(): float
{
    $usage = getrusage();

    return ($usage['ru_utime.tv_sec'] + $usage['ru_stime.tv_sec']) * 1000
        + ($usage['ru_utime.tv_usec'] + $usage['ru_stime.tv_usec']) / 1000;
}

/// Linear interpolation between ranks, exactly as measure.mjs does it. A nearest-rank
/// percentile would disagree with every other column at small sample counts.
function summarize(array $samples): array
{
    sort($samples);
    $count = count($samples);
    $at = function (float $fraction) use ($samples, $count): float {
        if ($count === 0) {
            return 0.0;
        }
        $rank = $fraction * ($count - 1);
        $low = (int) floor($rank);
        $high = (int) ceil($rank);
        if ($low === $high) {
            return $samples[$low];
        }

        return $samples[$low] + ($rank - $low) * ($samples[$high] - $samples[$low]);
    };

    return [
        'unit' => 'ms',
        'samples' => $count,
        'min' => $count > 0 ? $samples[0] : 0.0,
        'p50' => $at(0.5),
        'p95' => $at(0.95),
        'p99' => $at(0.99),
        'max' => $count > 0 ? $samples[$count - 1] : 0.0,
        'mean' => $count > 0 ? array_sum($samples) / $count : 0.0,
    ];
}

/// `--flag value` and bare `--flag`, the same shape measure.mjs parses, so one set of
/// arguments from run-bench.sh means the same thing to every column.
function parse_args(array $argv, array $defaults): array
{
    $values = $defaults;
    $count = count($argv);
    for ($index = 1; $index < $count; $index++) {
        if (! str_starts_with($argv[$index], '--')) {
            continue;
        }
        $name = substr($argv[$index], 2);
        $value = $argv[$index + 1] ?? null;
        if ($value === null || str_starts_with($value, '--')) {
            $values[$name] = 'true';
            continue;
        }
        $values[$name] = $value;
        $index++;
    }

    return $values;
}

$args = parse_args($argv, [
    'subscribers' => '10,50,100,250',
    'seconds' => '5',
    'hz' => '30',
    'payload' => '256',
    'saturate' => 'false',
    'out' => '',
    'reverb-port' => '9899',
    'reverb-key' => 'synqt-bench-key',
]);

$sizes = array_values(array_filter(array_map(
    fn ($part) => (int) trim($part),
    explode(',', $args['subscribers'])
), fn ($size) => $size > 0));
$seconds = max(1, (int) $args['seconds']);
$hz = max(1, (int) $args['hz']);
$payloadBytes = max(0, (int) $args['payload']);
$saturate = $args['saturate'] === 'true';
$port = (int) $args['reverb-port'];
$key = $args['reverb-key'];

echo "Laravel Reverb live path: {$seconds}s at ".($saturate ? 'saturation' : "{$hz} Hz").
    ", {$payloadBytes} byte payload, subscribers {$args['subscribers']}\n";

$baselineRss = resident_bytes();
$sweep = [];

foreach ($sizes as $subscriberCount) {
    $loop = Loop::get();
    $connector = new Connector($loop);
    $samples = [];
    $subscribed = 0;
    $measuring = false;
    $connections = [];
    // Written by the subscriber handler, read by nothing else. One process, one loop.
    $deliveredThisFrame = 0;
    // Null until the publisher exists, so a frame arriving early has nothing to write to.
    $publisherIn = null;

    for ($index = 0; $index < $subscriberCount; $index++) {
        $connector("ws://127.0.0.1:{$port}/app/{$key}?protocol=7&client=synqt-bench&version=1")
            ->then(function ($connection) use (
                &$samples, &$subscribed, &$measuring, &$connections,
                &$deliveredThisFrame, &$publisherIn, $saturate, $subscriberCount
            ) {
                $connections[] = $connection;
                $connection->on('message', function ($message) use (
                    $connection, &$samples, &$subscribed, &$measuring,
                    &$deliveredThisFrame, &$publisherIn, $saturate, $subscriberCount
                ) {
                    $envelope = json_decode((string) $message, true);
                    if (! is_array($envelope)) {
                        return;
                    }
                    $event = $envelope['event'] ?? '';
                    if ($event === 'pusher:connection_established') {
                        $connection->send(json_encode([
                            'event' => 'pusher:subscribe',
                            'data' => ['channel' => 'live'],
                        ]));

                        return;
                    }
                    if ($event === 'pusher_internal:subscription_succeeded') {
                        $subscribed++;

                        return;
                    }
                    if ($event !== 'frame' || ! $measuring) {
                        return;
                    }
                    // Reverb's envelope is the Pusher protocol's JSON, so the 8-byte stamp
                    // travels base64 inside it. The frame layout is the contract's. The
                    // encoding around it is what Reverb puts on the wire.
                    $data = json_decode($envelope['data'] ?? '{}', true);
                    $frame = base64_decode($data['b'] ?? '', true);
                    if ($frame === false || strlen($frame) < 8) {
                        return;
                    }
                    $stamp = unpack('P', substr($frame, 0, 8))[1];
                    $samples[] = (now_micros() - $stamp) / 1000;
                    if (! $saturate) {
                        return;
                    }
                    $deliveredThisFrame++;
                    if ($deliveredThisFrame < $subscriberCount || $publisherIn === null) {
                        return;
                    }
                    // The whole fleet has this frame, so the publisher may send the next one.
                    $deliveredThisFrame = 0;
                    fwrite($publisherIn, "next\n");
                    fflush($publisherIn);
                });
            }, function ($error) {
                fwrite(STDERR, "a subscriber did not connect: {$error->getMessage()}\n");
                exit(1);
            });
    }

    // Wait for the whole fleet to be subscribed before anything is published.
    $deadline = microtime(true) + 30;
    $waiter = $loop->addPeriodicTimer(0.05, function ($timer) use (
        &$subscribed, $subscriberCount, $loop, $deadline
    ) {
        if ($subscribed >= $subscriberCount || microtime(true) > $deadline) {
            $loop->cancelTimer($timer);
            $loop->stop();
        }
    });
    $loop->run();
    $loop->cancelTimer($waiter);

    if ($subscribed < $subscriberCount) {
        fwrite(STDERR, "subscribers did not come up at N={$subscriberCount}\n");
        exit(1);
    }
    $connectedRss = resident_bytes();

    // The publisher is a separate process, started here, because Laravel's broadcast path
    // blocks on a signed HTTP call and a blocking publisher on this loop would stall the
    // reads it is being timed against.
    $publisher = proc_open(
        ['php', __DIR__.'/publish.php', (string) $hz, (string) $seconds, (string) $payloadBytes,
            $saturate ? 'true' : 'false'],
        [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']],
        $pipes,
        __DIR__
    );
    stream_set_blocking($pipes[1], false);
    $publisherIn = $pipes[0];

    // The publisher says "warmed" when the discarded ticks are done. Measuring starts then,
    // so the warm-up never reaches the statistics.
    $ticks = 0;
    $cpuBefore = 0.0;
    $startedAt = 0.0;
    $finished = false;
    $pump = $loop->addPeriodicTimer(0.005, function () use (
        &$measuring, &$ticks, &$cpuBefore, &$startedAt, &$finished, $pipes, $loop, $saturate
    ) {
        $line = fgets($pipes[1]);
        if ($line === false) {
            return;
        }
        if (str_starts_with($line, 'warmed')) {
            $measuring = true;
            $cpuBefore = cpu_milliseconds();
            $startedAt = microtime(true);
            // Only now is the publisher told to open the measured window. It is blocked on
            // this line until the count is running, so no measured tick is published to nobody.
            fwrite($pipes[0], "go\n");
            fflush($pipes[0]);

            return;
        }
        if (str_starts_with($line, 'done')) {
            $ticks = (int) trim(substr($line, 5));
            // Let what is in flight land, or the tail of every run reads as loss that is
            // the harness stopping first. Saturation waited for each frame before
            // sending the next, so there is nothing in flight and the wait would only be
            // half a second of idle inside the elapsed time throughput is divided by.
            $drain = $saturate ? 0.0 : 0.5;
            $loop->addTimer($drain, function () use ($loop, &$finished) {
                $finished = true;
                $loop->stop();
            });
        }
    });
    $guard = $loop->addTimer($seconds + 90, fn () => $loop->stop());
    $loop->run();
    $loop->cancelTimer($pump);
    $loop->cancelTimer($guard);

    $elapsedSeconds = $startedAt > 0 ? microtime(true) - $startedAt : 0.001;
    $cpuMs = cpu_milliseconds() - $cpuBefore;
    $measuring = false;
    fclose($pipes[0]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($publisher);

    $delivered = count($samples);
    $entry = [
        'subscribers' => $subscriberCount,
        'propagation' => summarize($samples),
        'throughput_msgs_per_sec' => $delivered / max($elapsedSeconds, 0.001),
        'cpu_ms_per_1k' => $delivered > 0 ? ($cpuMs * 1000) / $delivered : 0.0,
        'rss_bytes_per_conn' => $subscriberCount > 0 && $connectedRss > $baselineRss
            ? ($connectedRss - $baselineRss) / $subscriberCount
            : 0.0,
        'rss_total_bytes' => $connectedRss,
        'delivered' => $delivered,
        'expected' => $ticks * $subscriberCount,
    ];
    printf("  N=%d  p50 %.3f ms  p99 %.3f ms  %.0f msg/s  delivered %d/%d\n",
        $entry['subscribers'], $entry['propagation']['p50'], $entry['propagation']['p99'],
        $entry['throughput_msgs_per_sec'], $entry['delivered'], $entry['expected']);
    $sweep[] = $entry;

    foreach ($connections as $connection) {
        $connection->close();
    }
    usleep(200000);
}

$document = [
    'benchmark' => 'vs-frameworks-live',
    'stack' => 'php-reverb',
    'path' => 'Laravel Reverb (ReactPHP), Pusher protocol over WebSockets',
    'php_version' => PHP_VERSION,
    'host' => 'linux '.trim(@file_get_contents('/proc/sys/kernel/osrelease') ?: ''),
    'arch' => php_uname('m'),
    'recorded' => gmdate('Y-m-d\TH:i:s\Z'),
    'rss_available' => true,
    'hz' => $saturate ? 0 : $hz,
    'saturated' => $saturate,
    'seconds' => $seconds,
    'payload_bytes' => $payloadBytes,
    'sweep' => $sweep,
];

if ($args['out'] !== '') {
    file_put_contents($args['out'],
        json_encode($document, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES)."\n");
    echo "\nwrote {$args['out']}\n";
}
