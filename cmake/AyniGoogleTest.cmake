# GoogleTest for native unit tests only (ADR-0012); never linked into Core or
# firmware. The release archive is pinned by hash. For offline builds, pass
# -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<extracted googletest-1.17.0>.

include_guard(GLOBAL)
include(FetchContent)

# Discover test lists after all builds finish. POST_BUILD otherwise runs ASan
# startup concurrently with several large links under the shared memory budget.
# CTest enumerates every suite; individual runtime deadlines remain unchanged.
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
# The monolithic unit binary enumerates over 1600 cases under ASan/LSan. Its
# default five-second process deadline cut off discovery in CI (#137).
set(AYNI_GTEST_DISCOVERY_TIMEOUT_SECONDS 30)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

# Build directories that name the same download directory share one copy of
# the archive, which CI keeps in its cache. A copy whose hash matches is not
# downloaded again.
set(AYNI_DOWNLOAD_DIR "" CACHE PATH "Directory that keeps downloaded third-party archives")
set(googletest_download_dir)
if(AYNI_DOWNLOAD_DIR)
    set(googletest_download_dir DOWNLOAD_DIR "${AYNI_DOWNLOAD_DIR}")
endif()

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    ${googletest_download_dir}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)
