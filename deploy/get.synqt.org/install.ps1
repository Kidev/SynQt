# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0
#
# Install the latest release of the SynQt command line tool (Windows).
#
# This script is what get.synqt.org serves at the /install.ps1 path, so this
# installs SynQt in PowerShell:
#
#   irm https://get.synqt.org/install.ps1 | iex
#
# It downloads the Windows asset for your architecture from the latest non
# prerelease on GitHub. The /releases/latest/download/<asset> path always
# resolves to the newest stable release, so this script self updates.
#
# The POSIX counterpart for Linux and macOS is https://get.synqt.org/install.sh
# (also served at the get.synqt.org root).
#
# Reading a script before piping it to a shell is strongly recommended. This one
# only downloads, extracts, and copies a single binary into a bin directory.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# GitHub requires TLS 1.2 or newer. Windows PowerShell 5.1 does not always
# negotiate it by default, so opt in explicitly (a no-op on PowerShell 7+,
# which already uses a modern default).
try {
    [Net.ServicePointManager]::SecurityProtocol = `
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
} catch {}

$owner = 'Kidev'
$repo  = 'SynQt'
$base  = "https://github.com/$owner/$repo/releases/latest/download"

# Resolve the true machine architecture, even when a 32-bit PowerShell host runs
# on 64-bit Windows (WOW64 sets PROCESSOR_ARCHITEW6432 to the real value).
$rawArch = $env:PROCESSOR_ARCHITEW6432
if ([string]::IsNullOrEmpty($rawArch)) { $rawArch = $env:PROCESSOR_ARCHITECTURE }

switch ($rawArch) {
    'AMD64' { $arch = 'x86_64' }
    'ARM64' { $arch = 'arm64'  }
    default {
        throw "Unsupported architecture: $rawArch. See " +
              "https://github.com/$owner/$repo/releases/latest for all builds."
    }
}

$asset = "synqt-windows-$arch.zip"
$url   = "$base/$asset"

# Where the binary lands. Override with $env:SYNQT_BIN; otherwise a per-user
# Programs directory that needs no administrator rights.
$binDir = $env:SYNQT_BIN
if ([string]::IsNullOrEmpty($binDir)) {
    $binDir = Join-Path $env:LOCALAPPDATA 'Programs\synqt'
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("synqt-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
try {
    $zip = Join-Path $tmp $asset

    Write-Host "Downloading $asset ..."
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

    Write-Host "Extracting ..."
    Expand-Archive -Path $zip -DestinationPath $tmp -Force

    $exe = Join-Path $tmp 'synqt.exe'
    if (-not (Test-Path $exe)) { throw "synqt.exe was not found inside $asset." }

    New-Item -ItemType Directory -Path $binDir -Force | Out-Null
    Copy-Item -Path $exe -Destination (Join-Path $binDir 'synqt.exe') -Force
}
finally {
    Remove-Item -Path $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Installed synqt to $(Join-Path $binDir 'synqt.exe')"

# Ensure the bin directory is on the user PATH (comparison is case-insensitive,
# matching Windows path semantics).
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$parts = @()
if (-not [string]::IsNullOrEmpty($userPath)) {
    $parts = $userPath -split ';' | Where-Object { $_ -ne '' }
}
if ($parts -notcontains $binDir) {
    $newPath = (@($binDir) + $parts) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    # Make it usable in this session too, without opening a new terminal.
    $env:Path = "$binDir;$env:Path"
    Write-Host "Added $binDir to your PATH. Open a new terminal for it to take effect everywhere."
}

Write-Host "Verify with: synqt doctor"
