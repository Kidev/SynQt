#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# ccache settings for SynQt's own test tree, sourced by tests/run-all.sh and
# tests/run-coverage.sh. cmake/SynQtBuildFlags.cmake routes the compiler through the cache.
#
# CCACHE_NOHASHDIR lets builds configured in different directories share entries. A cached
# object may then name another build tree as its compilation directory, which is acceptable
# for tests only. Inert when ccache is not installed.
export CCACHE_NOHASHDIR=1
