#!/usr/bin/env bash
# Run a complete existing test suite in the selected native configuration.
# The caller owns container lifetime and resource limits.
set -euo pipefail
preset=${1:?Expected a native preset}
case "$preset" in
    native-asan|native-release)
        export CC=gcc CXX=g++
        ;;
    native-tsan)
        export CC=gcc CXX=g++
        # ThreadSanitizer processes take several times the memory of the
        # others; keep their tests within the container's 3 GiB.
        if [ "${CTEST_PARALLEL_LEVEL:-1}" -gt 6 ]; then
            export CTEST_PARALLEL_LEVEL=6
        fi
        ;;
    native-debug)
        export CC=clang CXX=clang++
        ;;
    *) echo "Unsupported CI preset: $preset" >&2; exit 2 ;;
esac
# Keep native cache/PCH settings out of the ESP-IDF job's toolchain environment.
export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
# Room for all four native configurations; a 1G cache was already full.
export CCACHE_DIR=/ccache CCACHE_MAXSIZE=4G
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
# For choosing the parallelism and limits: the container's peak so far (cgroup v2).
if [ -r /sys/fs/cgroup/memory.peak ]; then
    echo "Container memory peak: $(($(cat /sys/fs/cgroup/memory.peak) / 1048576)) MiB"
fi
