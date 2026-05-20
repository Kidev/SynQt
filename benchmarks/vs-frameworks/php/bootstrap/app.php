<?php
// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The smallest Laravel application that can run `reverb:start` and `broadcast()`.
//
// Not the `laravel/laravel` skeleton. This column has no models, no views, no routes and no
// middleware, and a skeleton would commit forty files none of which the measurement reads.
// What is here is what Reverb and the broadcaster need, and `withExceptions()` is
// not optional despite being empty. It is what binds the exception handler the console
// kernel resolves before it runs anything.

use Illuminate\Foundation\Application;
use Illuminate\Foundation\Configuration\Exceptions;
use Illuminate\Foundation\Configuration\Middleware;

return Application::configure(basePath: dirname(__DIR__))
    ->withMiddleware(function (Middleware $middleware) {
        //
    })
    ->withExceptions(function (Exceptions $exceptions) {
        //
    })
    ->create();
