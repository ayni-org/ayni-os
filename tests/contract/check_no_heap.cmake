# Fails when a Core archive references a heap allocation symbol (ADR-0014).
# Placement new is inline and has no symbol, so constructing objects in
# caller-provided memory passes. Usage:
#   cmake -DNM=<nm> -DARCHIVES=<archive>[,<archive>...] -P check_no_heap.cmake

if(NOT NM)
    message(FATAL_ERROR "no nm tool: CMAKE_NM is empty")
endif()
string(REPLACE "," ";" archives "${ARCHIVES}")
# Allocators, and libc functions that return memory the caller must free.
# _FORTIFY_SOURCE and older glibc emit some under other names (__strdup,
# __asprintf_chk).
set(allocators
    malloc calloc realloc reallocarray free aligned_alloc posix_memalign memalign valloc pvalloc
    strdup strndup __strdup __strndup wcsdup asprintf vasprintf __asprintf_chk __vasprintf_chk
    getline getdelim open_memstream open_wmemstream)
list(JOIN allocators "|" allocator_pattern)
set(forbidden "^(${allocator_pattern}|_Znw.*|_Zna.*|_Zdl.*|_Zda.*)$")

set(found)
foreach(archive IN LISTS archives)
    execute_process(COMMAND "${NM}" -u "${archive}"
        OUTPUT_VARIABLE undefined ERROR_VARIABLE error RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${NM} -u ${archive} failed: ${error}")
    endif()
    string(REPLACE "\n" ";" lines "${undefined}")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line MATCHES "^U[ \t]+(.+)$" AND CMAKE_MATCH_1 MATCHES "${forbidden}")
            list(APPEND found "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()

if(found)
    list(REMOVE_DUPLICATES found)
    message(FATAL_ERROR "Core references heap allocator symbols: ${found}")
endif()
message(STATUS "no heap allocator symbols in: ${archives}")
