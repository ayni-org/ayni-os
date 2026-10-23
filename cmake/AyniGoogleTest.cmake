# GoogleTest for native unit tests only (ADR-0012); never linked into Core or
# firmware. The release archive is pinned by hash. For offline builds, pass
# -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<extracted googletest-1.17.0>.

include_guard(GLOBAL)
include(FetchContent)

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
