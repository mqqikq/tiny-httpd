#ifndef FILES_H
#define FILES_H

#include <stddef.h>
#include <sys/stat.h>

typedef enum {
    FILE_RESOLVE_OK,             /* out_fullpath is a regular, readable file */
    FILE_RESOLVE_FORBIDDEN,      /* escapes doc_root, or unreadable for permission reasons */
    FILE_RESOLVE_NOT_FOUND,      /* no such file (and no usable index.html) */
    FILE_RESOLVE_BAD_REQUEST,    /* malformed request-target (bad percent-encoding, no leading '/') */
} file_resolve_result_t;

/* Resolves an HTTP request-target (e.g. "/a/b.html?x=1") against doc_root,
 * rejecting any attempt to escape the document root (via "..", absolute
 * paths smuggled through encoding, or symlinks). Directories are resolved
 * to "<dir>/index.html" if present.
 *
 * doc_root must already be an absolute, canonical path (no symlinks, no
 * trailing slash) -- e.g. produced once at startup via realpath().
 *
 * On FILE_RESOLVE_OK, out_fullpath holds the canonical filesystem path and
 * *out_st holds its stat() result. */
file_resolve_result_t resolve_path(const char *doc_root, const char *request_target,
                                    char *out_fullpath, size_t out_size,
                                    struct stat *out_st);

#endif /* FILES_H */
