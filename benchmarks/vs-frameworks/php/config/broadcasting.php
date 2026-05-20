<?php
// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The broadcast connection the publisher uses. `publish.php` asks for it by name and gets
// Laravel's real Reverb broadcaster, which is the point of the column. The frames go out
// through the path an application uses, not through a socket the harness opened itself.
//
// The port is overridden at run time, because run-bench.sh picks a free one rather than
// assuming 9899 is available.

return [
    'default' => 'reverb',

    'connections' => [
        'reverb' => [
            'driver' => 'reverb',
            'key' => 'synqt-bench-key',
            'secret' => 'synqt-bench-secret',
            'app_id' => 'synqt-bench',
            'options' => [
                'host' => '127.0.0.1',
                'port' => (int) (getenv('SYNQT_REVERB_PORT') ?: 9899),
                'scheme' => 'http',
                'useTLS' => false,
            ],
            'client_options' => [],
        ],
    ],
];
