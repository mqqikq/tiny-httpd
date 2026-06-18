#include "http_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Header section is capped well below typical buffer sizes so a single
 * request can't exhaust memory; connection.c enforces the hard byte limit
 * before this is ever called. */
#define HEADER_SECTION_MAX (16 * 1024)

static const char *find_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

static int parse_request_line(const char *line, size_t line_len, http_request_t *req) {
    /* "METHOD SP request-target SP HTTP-version" */
    const char *p = line;
    const char *end = line + line_len;

    const char *sp1 = memchr(p, ' ', (size_t)(end - p));
    if (sp1 == NULL) return -1;
    size_t method_len = (size_t)(sp1 - p);
    if (method_len == 0 || method_len >= sizeof(req->method)) return -1;
    memcpy(req->method, p, method_len);
    req->method[method_len] = '\0';

    p = sp1 + 1;
    const char *sp2 = memchr(p, ' ', (size_t)(end - p));
    if (sp2 == NULL) return -1;
    size_t path_len = (size_t)(sp2 - p);
    if (path_len == 0 || path_len >= sizeof(req->path)) return -1;
    memcpy(req->path, p, path_len);
    req->path[path_len] = '\0';

    p = sp2 + 1;
    size_t version_len = (size_t)(end - p);
    if (version_len == 0 || version_len >= sizeof(req->version)) return -1;
    if (strncmp(p, "HTTP/", 5) != 0) return -1;
    memcpy(req->version, p, version_len);
    req->version[version_len] = '\0';

    if (strcmp(req->version, "HTTP/1.0") != 0 && strcmp(req->version, "HTTP/1.1") != 0) {
        return -1;
    }
    return 0;
}

static int parse_header_line(const char *line, size_t line_len, http_header_t *out) {
    const char *colon = memchr(line, ':', line_len);
    if (colon == NULL) return -1;

    size_t name_len = (size_t)(colon - line);
    if (name_len == 0 || name_len >= sizeof(out->name)) return -1;
    memcpy(out->name, line, name_len);
    out->name[name_len] = '\0';

    const char *vstart = colon + 1;
    const char *vend = line + line_len;
    while (vstart < vend && (*vstart == ' ' || *vstart == '\t')) vstart++;
    while (vend > vstart && (vend[-1] == ' ' || vend[-1] == '\t')) vend--;

    size_t value_len = (size_t)(vend - vstart);
    if (value_len >= sizeof(out->value)) return -1;
    memcpy(out->value, vstart, value_len);
    out->value[value_len] = '\0';

    return 0;
}

http_parse_result_t http_parse_request(const char *buf, size_t len,
                                        http_request_t *out_req,
                                        size_t *out_consumed) {
    static const char terminator[] = "\r\n\r\n";
    const char *header_end = NULL;

    size_t scan_len = len < HEADER_SECTION_MAX ? len : HEADER_SECTION_MAX;
    for (size_t i = 0; i + 3 < scan_len; i++) {
        if (memcmp(buf + i, terminator, 4) == 0) {
            header_end = buf + i;
            break;
        }
    }

    if (header_end == NULL) {
        if (len >= HEADER_SECTION_MAX) {
            return HTTP_PARSE_TOO_LARGE;
        }
        return HTTP_PARSE_INCOMPLETE;
    }

    memset(out_req, 0, sizeof(*out_req));

    const char *cursor = buf;
    const char *section_end = header_end; /* exclusive, points at the blank line's \r\n */

    const char *line_end = find_crlf(cursor, (size_t)(section_end - cursor) + 2);
    if (line_end == NULL || line_end > section_end) return HTTP_PARSE_ERROR;

    if (parse_request_line(cursor, (size_t)(line_end - cursor), out_req) != 0) {
        return HTTP_PARSE_ERROR;
    }
    cursor = line_end + 2;

    while (cursor < section_end) {
        const char *next = find_crlf(cursor, (size_t)(section_end - cursor) + 2);
        if (next == NULL || next > section_end) return HTTP_PARSE_ERROR;

        size_t line_len = (size_t)(next - cursor);
        if (line_len == 0) break; /* shouldn't happen before section_end, defensive */

        if (out_req->header_count >= HTTP_MAX_HEADERS) {
            return HTTP_PARSE_TOO_LARGE;
        }
        if (parse_header_line(cursor, line_len, &out_req->headers[out_req->header_count]) != 0) {
            return HTTP_PARSE_ERROR;
        }
        out_req->header_count++;
        cursor = next + 2;
    }

    *out_consumed = (size_t)(header_end - buf) + 4;
    return HTTP_PARSE_OK;
}

const char *http_get_header(const http_request_t *req, const char *name) {
    for (size_t i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}
