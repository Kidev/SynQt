#!/bin/sh
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0
#
# Install the latest release of the SynQt command line tool.
#
# This script is what get.synqt.org serves: it is published at both the root
# and the /install.sh path (get.synqt.org's index.html is a byte for byte copy
# of this file), so either of these installs SynQt:
#
#   curl -fsSL https://get.synqt.org | sh
#   curl -fsSL https://get.synqt.org/install.sh | sh
#
# This downloads the asset for your operating system and architecture from the
# latest non prerelease on GitHub. The /releases/latest/download/<asset> path
# always resolves to the newest stable release, so this script self updates.
#
# Reading a script before piping it to a shell is strongly recommended. This one
# only downloads, extracts, and copies a single binary into a bin directory.

set -eu

OWNER="Kidev"
REPO="SynQt"
BASE="https://github.com/${OWNER}/${REPO}/releases/latest/download"

os="$(uname -s)"
arch="$(uname -m)"

case "$os" in
  Linux)  os_tag="linux"  ; ext="tar.gz" ;;
  Darwin) os_tag="macos"  ; ext="tar.gz" ;;
  *)
    echo "Unsupported operating system: $os" >&2
    echo "See ${BASE%/download} for all builds, including Windows." >&2
    exit 1
    ;;
esac

case "$arch" in
  x86_64|amd64)   arch_tag="x86_64" ;;
  arm64|aarch64)  arch_tag="arm64"  ;;
  *)
    echo "Unsupported architecture: $arch" >&2
    exit 1
    ;;
esac

asset="synqt-${os_tag}-${arch_tag}.${ext}"
url="${BASE}/${asset}"

bindir="${SYNQT_BIN:-$HOME/.local/bin}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading ${asset} ..."
if command -v curl >/dev/null 2>&1; then
  curl -fSL "$url" -o "${tmp}/${asset}"
elif command -v wget >/dev/null 2>&1; then
  wget -O "${tmp}/${asset}" "$url"
else
  echo "Need curl or wget to download." >&2
  exit 1
fi

echo "Extracting ..."
tar -xzf "${tmp}/${asset}" -C "$tmp"

mkdir -p "$bindir"
install -m 0755 "${tmp}/synqt" "${bindir}/synqt"

echo "Installed synqt to ${bindir}/synqt"
case ":${PATH}:" in
  *":${bindir}:"*) : ;;
  *) echo "Add ${bindir} to your PATH, then open a new shell." ;;
esac
echo "Verify with: synqt doctor"
