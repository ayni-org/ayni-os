#!/bin/bash
set -euo pipefail
probe_dir="$(cd "$(dirname "$0")" && pwd)"
project_dir="$(dirname "$probe_dir")"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/Users/danny/Documents/Codex/2026-10-20/task/.tools/espressif}"
export PATH="/Users/danny/.local/share/uv/python/cpython-3.14.5-macos-aarch64-none/bin:$PATH"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
export CTEST_PARALLEL_LEVEL="${CTEST_PARALLEL_LEVEL:-2}"
source "${IDF_PATH:-/Users/danny/Documents/Codex/2026-10-20/task/.tools/esp-idf}/export.sh" >/dev/null
idf.py -C "$probe_dir" -B "$project_dir/build/motor-probe" reconfigure
cmake --build "$project_dir/build/motor-probe" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
