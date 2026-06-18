/* Unit tests for files.c's resolve_path(): the path-traversal/symlink-escape
 * guard is the single most security-critical piece of this server, so it
 * gets its own fixture with a real temp directory tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                              \
    do {                                                               \
        if (cond) {                                                    \
            printf("  PASS: %s\n", msg);                               \
        } else {                                                       \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            g_failures++;                                              \
        }                                                               \
    } while (0)

static void write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(contents, f);
    fclose(f);
}

int main(void) {
    char tmpl[] = "/tmp/httpd_test_XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    if (!tmpdir) { perror("mkdtemp"); return 1; }

    char root[4096];
    snprintf(root, sizeof(root), "%s/root", tmpdir);
    mkdir(root, 0755);

    char sub[4096];
    snprintf(sub, sizeof(sub), "%s/sub", root);
    mkdir(sub, 0755);

    char index_path[4096], page_path[4096], dir_index_path[4096];
    snprintf(index_path, sizeof(index_path), "%s/index.html", root);
    snprintf(page_path, sizeof(page_path), "%s/sub/page.html", root);
    snprintf(dir_index_path, sizeof(dir_index_path), "%s/sub/index.html", root);
    write_file(index_path, "home\n");
    write_file(page_path, "page\n");
    write_file(dir_index_path, "sub index\n");

    /* secret file OUTSIDE the document root */
    char secret[4096];
    snprintf(secret, sizeof(secret), "%s/secret.txt", tmpdir);
    write_file(secret, "top secret\n");

    /* symlink inside root pointing outside root -- must not be servable */
    char escape_link[4096];
    snprintf(escape_link, sizeof(escape_link), "%s/escape", root);
    if (symlink(secret, escape_link) != 0) { perror("symlink"); return 1; }

    char canonical_root[4096];
    if (realpath(root, canonical_root) == NULL) { perror("realpath(root)"); return 1; }

    char out[4096];
    struct stat st;
    file_resolve_result_t r;

    printf("test_serves_existing_file\n");
    r = resolve_path(canonical_root, "/sub/page.html", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_OK, "page.html resolves OK");

    printf("test_directory_index\n");
    r = resolve_path(canonical_root, "/sub/", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_OK, "/sub/ resolves to sub/index.html");
    r = resolve_path(canonical_root, "/sub", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_OK, "/sub (no trailing slash) also resolves via index.html");

    printf("test_missing_file\n");
    r = resolve_path(canonical_root, "/nope.html", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_NOT_FOUND, "missing file is NOT_FOUND");

    printf("test_dotdot_traversal_blocked\n");
    r = resolve_path(canonical_root, "/../secret.txt", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_FORBIDDEN, "../secret.txt is FORBIDDEN, not served");

    printf("test_deep_dotdot_traversal_blocked\n");
    r = resolve_path(canonical_root, "/sub/../../secret.txt", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_FORBIDDEN, "sub/../../secret.txt is FORBIDDEN");

    printf("test_encoded_dotdot_blocked\n");
    r = resolve_path(canonical_root, "/%2e%2e/secret.txt", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_FORBIDDEN, "percent-encoded ../ is decoded then still blocked");

    printf("test_symlink_escape_blocked\n");
    r = resolve_path(canonical_root, "/escape", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_FORBIDDEN, "symlink pointing outside root is FORBIDDEN");

    printf("test_null_byte_rejected\n");
    r = resolve_path(canonical_root, "/index.html%00.txt", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_BAD_REQUEST, "embedded NUL via %%00 is BAD_REQUEST");

    printf("test_missing_leading_slash_rejected\n");
    r = resolve_path(canonical_root, "index.html", out, sizeof(out), &st);
    CHECK(r == FILE_RESOLVE_BAD_REQUEST, "request-target without leading '/' is BAD_REQUEST");

    if (g_failures == 0) {
        printf("All files.c tests passed.\n");
        return 0;
    }
    printf("%d files.c test(s) FAILED.\n", g_failures);
    return 1;
}
