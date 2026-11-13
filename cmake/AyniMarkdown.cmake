# Optional Markdown-to-speech boundary. Core and the base SDK do not link this parser.
include_guard(GLOBAL)
include(FetchContent)
set(AYNI_DOWNLOAD_DIR "" CACHE PATH "Directory that keeps downloaded third-party archives")
set(md4c_download_dir)
if(AYNI_DOWNLOAD_DIR)
    set(md4c_download_dir DOWNLOAD_DIR "${AYNI_DOWNLOAD_DIR}")
endif()
FetchContent_Declare(md4c
    URL https://codeload.github.com/mity/md4c/tar.gz/refs/tags/release-0.5.2
    URL_HASH SHA256=55d0111d48fb11883aaee91465e642b8b640775a4d6993c2d0e7a8092758ef21
    ${md4c_download_dir}
    SOURCE_SUBDIR ayni-source-only
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(md4c)
# The upstream HTML renderer uses the same entity map; no HTML renderer or executable is built.
add_library(ayni_markdown STATIC "${md4c_SOURCE_DIR}/src/md4c.c" "${md4c_SOURCE_DIR}/src/entity.c")
target_include_directories(ayni_markdown PUBLIC "${md4c_SOURCE_DIR}/src")
if(ESP_PLATFORM AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # Xtensa GCC 15 -Os loses md_is_link_label's success invariant: every TRUE path
    # initializes p_beg_line_index. Retain the diagnostic, only relax this vendor warning.
    target_compile_options(ayni_markdown PRIVATE -Wno-error=maybe-uninitialized)
endif()
