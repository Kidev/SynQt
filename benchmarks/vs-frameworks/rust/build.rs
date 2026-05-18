// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Stamp the rustc that compiled this column into the binary.
//
// Cargo.toml's `rust-version` is the pin, which is the oldest toolchain this is allowed to
// build on and not the one that produced the number. A baseline has to name the runtime that
// made it, so the result file carries what this reads rather than the floor.

fn main() {
    let rustc = std::env::var("RUSTC").unwrap_or_else(|_| "rustc".into());
    let version = std::process::Command::new(rustc)
        .arg("--version")
        .output()
        .ok()
        .and_then(|out| String::from_utf8(out.stdout).ok())
        .map(|line| line.trim().trim_start_matches("rustc ").to_string())
        .unwrap_or_else(|| "unknown".into());
    println!("cargo:rustc-env=SYNQT_RUSTC_VERSION={version}");
    println!("cargo:rerun-if-changed=build.rs");
}
