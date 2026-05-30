<?php
// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The publisher half of the Laravel Reverb column. It goes through Laravel's broadcast path,
// which is what an application would do, and that path is a signed HTTP call into the Reverb
// process rather than a write to a socket this process holds.
//
// Its own process for that reason. Laravel's broadcaster blocks on the HTTP call, and a
// blocking publisher sharing a loop with the subscribers would stall their reads and put the
// harness's own latency into the measurement. That is the mistake the Action Cable column
// found the hard way (see the README).
//
// The stamp is hrtime(true), which is CLOCK_MONOTONIC on Linux and therefore system-wide:
// the subscriber process reads it back off the same clock, so the interval is still an
// interval across the two. See COLUMN-CONTRACT.md.

use Illuminate\Contracts\Console\Kernel;
use Illuminate\Support\Facades\Broadcast;

require __DIR__.'/vendor/autoload.php';

$app = require_once __DIR__.'/bootstrap/app.php';
$app->make(Kernel::class)->bootstrap();

$hz = (int) ($argv[1] ?? 30);
$seconds = (int) ($argv[2] ?? 5);
$payloadBytes = (int) ($argv[3] ?? 256);
// Saturation has to reach this process, not only the one that spawned it. While it did not,
// this side paced every measured window at $hz and the subscriber process still recorded the
// run as saturated. The Reverb row of a saturation sweep was a 30 Hz result under a heading
// that said otherwise, and it read as Reverb topping out at exactly 30 frames a second.
$saturate = ($argv[4] ?? 'false') === 'true';
$payload = str_repeat('x', $payloadBytes);

$broadcaster = Broadcast::connection('reverb');
$publish = function () use ($broadcaster, $payload) {
    // hrtime(true) is nanoseconds. The contract's stamp is microseconds.
    $stamp = intdiv(hrtime(true), 1000);
    $frame = pack('P', $stamp).$payload;
    $broadcaster->broadcast(['live'], 'frame', ['b' => base64_encode($frame)]);
};

// The warm-up, then the measured window. The subscriber process is the one that decides
// which is which. This side publishes at the rate for both.
$warmup = min($hz, 30);
for ($tick = 0; $tick < $warmup; $tick++) {
    $publish();
    usleep(intdiv(1000000, $hz));
}
// Say the warm-up is over, then WAIT to be told to start. Without the second half the
// measured window opens before the subscriber process has flipped to measuring, and the
// first tick is published to nobody counting. The run then reports N frames short and reads
// as loss that never happened.
fwrite(STDOUT, "warmed\n");
fgets(STDIN);

$started = hrtime(true);
if ($saturate) {
    // The closed loop COLUMN-CONTRACT.md asks for. Publish, wait for the whole fleet to have
    // that frame, publish again. The fleet lives in the subscriber process, so "the fleet has
    // it" arrives here as a line on stdin rather than as a counter this process can read. The
    // pipe costs a few tens of microseconds per frame and sits between frames, outside the
    // stamp-to-receipt interval every sample is measured over.
    $ticks = 0;
    $deadline = $started + (int) ($seconds * 1e9);
    while (hrtime(true) < $deadline) {
        $publish();
        $ticks++;
        // A frame that never lands everywhere would otherwise park this process until the
        // subscriber's own guard fires 90 seconds later, so it is a bounded wait that says
        // what went wrong.
        $readable = [STDIN];
        $writable = [];
        $except = [];
        if (stream_select($readable, $writable, $except, 5) < 1) {
            fwrite(STDERR, "a frame never reached every subscriber\n");
            exit(1);
        }
        if (fgets(STDIN) === false) {
            break;
        }
    }
} else {
    $ticks = $hz * $seconds;
    for ($tick = 0; $tick < $ticks; $tick++) {
        $publish();
        $due = $started + (int) (($tick + 1) * 1e9 / $hz);
        $wait = $due - hrtime(true);
        if ($wait > 0) {
            usleep((int) ($wait / 1000));
        }
    }
}
fwrite(STDOUT, "done $ticks\n");
