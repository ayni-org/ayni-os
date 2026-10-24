#!/usr/bin/env bash
# Run a complete existing test suite in the selected native configuration.
# The caller owns container lifetime and resource limits.
set -euo pipefail
preset=${1:?Expected a native preset}
case "$preset" in
    native-asan|native-release|native-tsan)
        export CC=gcc CXX=g++
        ;;
    native-debug)
        export CC=clang CXX=clang++
        ;;
    *) echo "Unsupported CI preset: $preset" >&2; exit 2 ;;
esac
# Keep native cache/PCH settings out of the ESP-IDF job's toolchain environment.
export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
export CCACHE_DIR=/ccache CCACHE_MAXSIZE=1G
export CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
cmake --version
"$CC" --version
ccache --zero-stats
cmake --preset "$preset" -DAYNI_DOWNLOAD_DIR=/downloads
cmake --build --preset "$preset"
ccache --show-stats
ctest --preset "$preset"
if [ "$preset" = native-debug ]; then
    build_dir=build/native
else
    build_dir="build/$preset"
fi
"$build_dir/targets/simulator/ayni-sim"
