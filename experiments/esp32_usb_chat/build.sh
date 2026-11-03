#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")" && pwd)"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/Users/danny/Documents/Codex/2026-10-20/task/.tools/espressif}"
export PATH="/Users/danny/.local/share/uv/python/cpython-3.14.5-macos-aarch64-none/bin:$PATH"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
export CTEST_PARALLEL_LEVEL="${CTEST_PARALLEL_LEVEL:-2}"
export PYTHONDONTWRITEBYTECODE=1
source "${IDF_PATH:-/Users/danny/Documents/Codex/2026-10-20/task/.tools/esp-idf}/export.sh" >/dev/null
mkdir -p "$project_dir/build"
idf.py -C "$project_dir/firmware" -B "$project_dir/build/motor-firmware" reconfigure
cmake --build "$project_dir/build/motor-firmware" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
xcrun swiftc -module-cache-path "$project_dir/build/swift-cache" \
    -o "$project_dir/build/AyniUSBChat.next" "$project_dir/desktop/App.swift"
mv "$project_dir/build/AyniUSBChat.next" "$project_dir/build/AyniUSBChat"
