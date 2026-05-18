// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The Go column's own module. The toolchain line pins what this was developed against, so a
// newer Go is a deliberate bump and not a surprise in a number.
//
// One dependency: coder/websocket, rather than the unmaintained gorilla/websocket. Nothing
// else, because this is a floor column and a router or a framework here would be measuring
// the router.
module synqt/benchmarks/vs-frameworks/go

go 1.26

require github.com/coder/websocket v1.8.15
