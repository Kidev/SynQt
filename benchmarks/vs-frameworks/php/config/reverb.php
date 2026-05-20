<?php
// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The Reverb server and the one application it serves, written as literals rather than read
// from a .env, for the reason config/app.php gives. Nothing here is a secret, and a .env is
// a file this repository correctly refuses to commit.
//
// SYNQT_REVERB_PORT is the one thing that moves, because run-bench.sh picks a free port
// rather than assuming one is available.

return [
    'default' => 'reverb',

    'servers' => [
        'reverb' => [
            'host' => '127.0.0.1',
            'port' => (int) (getenv('SYNQT_REVERB_PORT') ?: 9899),
            'path' => '',
            'hostname' => '127.0.0.1',
            'options' => [
                'tls' => [],
            ],
            'max_request_size' => 10_000,
            'scaling' => [
                // Off. Scaling puts a Redis channel between two Reverb processes, and a
                // column measuring Redis would not be measuring Reverb. Every other column
                // in this table fans out from the process holding the sockets.
                'enabled' => false,
                'channel' => 'reverb',
                'server' => [],
            ],
            'pulse_ingest_interval' => 15,
            'telescope_ingest_interval' => 15,
        ],
    ],

    'apps' => [
        'provider' => 'config',
        'apps' => [
            [
                'key' => 'synqt-bench-key',
                'secret' => 'synqt-bench-secret',
                'app_id' => 'synqt-bench',
                'options' => [
                    'host' => '127.0.0.1',
                    'port' => (int) (getenv('SYNQT_REVERB_PORT') ?: 9899),
                    'scheme' => 'http',
                    'useTLS' => false,
                ],
                'allowed_origins' => ['*'],
                'ping_interval' => 60,
                'activity_timeout' => 30,
                'max_message_size' => 10_000,
            ],
        ],
    ],
];
