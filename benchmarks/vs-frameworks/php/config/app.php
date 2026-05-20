<?php
// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The application settings Reverb's host needs, written as literals rather than read from a
// .env. There is nothing secret here to keep out of the repository. The key below encrypts
// nothing this column stores, and the column is a loopback server carrying bytes it
// generated. A real Laravel application reads all of this from the environment.

return [
    'name' => 'synqt-bench',
    'env' => 'production',
    'debug' => false,
    'key' => 'base64:c3lucXQtYmVuY2htYXJrLW5ldmVyLWEtZGVwbG95bWVudA==',
    'cipher' => 'AES-256-CBC',
    'timezone' => 'UTC',
    'locale' => 'en',
    'fallback_locale' => 'en',
    'faker_locale' => 'en_US',
];

// No 'providers' and no 'aliases'. Laravel 11 moved the provider list out of
// this file, and setting an empty one here overrides the framework's own defaults. The
// container then cannot resolve `files`, and `reverb:start` dies before it binds a port.
