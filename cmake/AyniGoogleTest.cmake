# GoogleTest for native unit tests only (ADR-0012); never linked into Core or
# firmware. The release archive is pinned by hash. For offline builds, pass
# -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<extracted googletest-1.17.0>.

include_guard(GLOBAL)
include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)
