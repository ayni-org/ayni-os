#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")" && pwd)"
profile="${1:---wifi}"
if [[ "$profile" != --wifi ]]; then
    echo 'Usage: build.sh [--wifi]' >&2
    exit 2
fi
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/Users/danny/Documents/Codex/2026-10-20/task/.tools/espressif}"
export PATH="/Users/danny/.local/share/uv/python/cpython-3.14.5-macos-aarch64-none/bin:$PATH"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
export CTEST_PARALLEL_LEVEL="${CTEST_PARALLEL_LEVEL:-2}"
export PYTHONDONTWRITEBYTECODE=1
source "${IDF_PATH:-/Users/danny/Documents/Codex/2026-10-20/task/.tools/esp-idf}/export.sh" >/dev/null
mkdir -p "$project_dir/build"
if [[ "$profile" == --wifi ]]; then
    assets="$project_dir/build/assets"
    mkdir -p "$assets"
    if [[ ! -f "$assets/unifont-16.0.04.hex.gz" ]]; then
        curl --fail --show-error --location 'https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz' \
            -o "$assets/unifont-16.0.04.hex.gz"
    fi
    python "$project_dir/prepare_font.py" "$assets/unifont-16.0.04.hex.gz" "$assets/ayni_font.bin"
    build_dir="$project_dir/build/wifi-firmware"
    # This profile is fully specified by the two tracked defaults, not menuconfig.
    # Regenerate only its disposable config so edited defaults actually take effect.
    if [[ "$project_dir/firmware/sdkconfig.defaults" -nt "$build_dir/sdkconfig" ||
          "$project_dir/firmware/sdkconfig.wifi" -nt "$build_dir/sdkconfig" ]]; then
        rm -f "$build_dir/sdkconfig"
    fi
    idf.py -C "$project_dir/firmware" -B "$build_dir" \
        -DAYNI_WIFI_PROFILE=ON -DSDKCONFIG="$build_dir/sdkconfig" \
        "-DSDKCONFIG_DEFAULTS=$project_dir/firmware/sdkconfig.defaults;$project_dir/firmware/sdkconfig.wifi" reconfigure
    cmake --build "$build_dir" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
fi
