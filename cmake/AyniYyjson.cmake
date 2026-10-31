# Optional JSON boundary support; Core and the basic SDK do not require it.
include_guard(GLOBAL)
include(FetchContent)
set(AYNI_DOWNLOAD_DIR "" CACHE PATH "Directory that keeps downloaded third-party archives")
set(yyjson_download_dir)
if(AYNI_DOWNLOAD_DIR)
    set(yyjson_download_dir DOWNLOAD_DIR "${AYNI_DOWNLOAD_DIR}")
endif()
set(YYJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YYJSON_BUILD_MISC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yyjson
    URL https://github.com/ibireme/yyjson/archive/refs/tags/0.10.0.tar.gz
    DOWNLOAD_NAME yyjson-0.10.0.tar.gz
    URL_HASH SHA256=0d901cb2c45c5586e3f3a4245e58c2252d6b24bf4b402723f6179523d389b165
    ${yyjson_download_dir}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(yyjson)
