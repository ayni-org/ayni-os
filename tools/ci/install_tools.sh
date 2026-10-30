#!/usr/bin/env bash
# The tracked recipe is the local image cache key. Bump this recipe to refresh
# packages even when the package list has not changed (for example, security updates).
# Recipe revision: 2026-10-26
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates git python3 cmake ninja-build ccache g++ \
    clang-18 clang-format-18 clang-tidy-18 libcurl4-openssl-dev libsqlite3-dev
# Keep the compiler names used by the existing presets and workflow.
ln -s /usr/bin/clang-18 /usr/local/bin/clang
ln -s /usr/bin/clang++-18 /usr/local/bin/clang++
rm -rf /var/lib/apt/lists/*
