#include "files.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Decodes %XX percent-escapes and rejects '\0' or malformed escapes.
 * Decodes in place; src and dst may be the same buffer. Returns 0 on
 * success, -1 on malformed input. */
static int url_decode(const char *src, char *dst, size_t dst_size) {
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        char c = src[i];
        if (c == '?' || c == '#') {
            break; /* strip query string / fragment */
        }
        if (c == '%') {
            if (!isxdigit((unsigned char)src[i + 1]) || !isxdigit((unsigned char)src[i + 2])) {
                return -1;
            }
            char hex[3] = {src[i + 1], src[i + 2], '\0'};
            int value = (int)strtol(hex, NULL, 16);
            if (value == 0) {
                return -1; /* embedded NUL */
            }
            if (out + 1 >= dst_size) return -1;
            dst[out++] = (char)value;
            i += 2;
        } else {
            if (out + 1 >= dst_size) return -1;
            dst[out++] = c;
        }
    }
    dst[out] = '\0';
    return 0;
}

file_resolve_result_t resolve_path(const char *doc_root, const char *request_target,
                                    char *out_fullpath, size_t out_size,
                                    struct stat *out_st) {
    if (request_target[0] != '/') {
        return FILE_RESOLVE_BAD_REQUEST;
    }

    char decoded[2048];
    if (url_decode(request_target, decoded, sizeof(decoded)) != 0) {
        return FILE_RESOLVE_BAD_REQUEST;
    }

    char joined[4096];
    int n = snprintf(joined, sizeof(joined), "%s%s", doc_root, decoded);
    if (n < 0 || (size_t)n >= sizeof(joined)) {
        return FILE_RESOLVE_BAD_REQUEST;
    }

    char canonical[4096];
    if (realpath(joined, canonical) == NULL) {
        return (errno == EACCES) ? FILE_RESOLVE_FORBIDDEN : FILE_RESOLVE_NOT_FOUND;
    }

    size_t root_len = strlen(doc_root);
    int within_root = strncmp(canonical, doc_root, root_len) == 0 &&
                       (canonical[root_len] == '\0' || canonical[root_len] == '/');
    if (!within_root) {
        return FILE_RESOLVE_FORBIDDEN;
    }

    struct stat st;
    if (stat(canonical, &st) != 0) {
        return FILE_RESOLVE_NOT_FOUND;
    }

    if (S_ISDIR(st.st_mode)) {
        char with_index[4096];
        n = snprintf(with_index, sizeof(with_index), "%s/index.html", canonical);
        if (n < 0 || (size_t)n >= sizeof(with_index)) {
            return FILE_RESOLVE_NOT_FOUND;
        }
        if (realpath(with_index, canonical) == NULL || stat(canonical, &st) != 0 ||
            !S_ISREG(st.st_mode)) {
            return FILE_RESOLVE_NOT_FOUND;
        }
        /* re-check containment in case index.html itself is a symlink */
        within_root = strncmp(canonical, doc_root, root_len) == 0 &&
                      (canonical[root_len] == '\0' || canonical[root_len] == '/');
        if (!within_root) {
            return FILE_RESOLVE_FORBIDDEN;
        }
    } else if (!S_ISREG(st.st_mode)) {
        return FILE_RESOLVE_FORBIDDEN;
    }

    if (strlen(canonical) >= out_size) {
        return FILE_RESOLVE_NOT_FOUND;
    }
    strcpy(out_fullpath, canonical);
    *out_st = st;
    return FILE_RESOLVE_OK;
}
