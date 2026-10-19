/* Deliberately duplicates a string: strdup allocates without naming malloc. */
#define _POSIX_C_SOURCE 200809L
#include <string.h>

char *ayni_heap_probe_strdup(const char *text);

char *ayni_heap_probe_strdup(const char *text) {
    return strdup(text);
}
