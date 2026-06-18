#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#define HTTP_MAX_HEADERS    32
#define HTTP_METHOD_LEN     16
#define HTTP_PATH_LEN       2048
#define HTTP_VERSION_LEN    16
#define HTTP_HEADER_NAME_LEN  64
#define HTTP_HEADER_VALUE_LEN 512

typedef struct {
    char name[HTTP_HEADER_NAME_LEN];
    char value[HTTP_HEADER_VALUE_LEN];
} http_header_t;

typedef struct {
    char method[HTTP_METHOD_LEN];
    char path[HTTP_PATH_LEN];     /* raw request-target, percent-encoding intact */
    char version[HTTP_VERSION_LEN];
    http_header_t headers[HTTP_MAX_HEADERS];
    size_t header_count;
} http_request_t;

typedef enum {
    HTTP_PARSE_OK = 0,         /* a full request was parsed */
    HTTP_PARSE_INCOMPLETE,     /* need more bytes (no \r\n\r\n yet) */
    HTTP_PARSE_ERROR,          /* malformed request line/headers */
    HTTP_PARSE_TOO_LARGE,      /* header section exceeds buffer capacity */
} http_parse_result_t;

/* Attempts to parse one HTTP request out of buf[0..len).
 * On HTTP_PARSE_OK, *out_consumed is set to the number of bytes that make up
 * the request line + headers + the terminating blank line (i.e. where the
 * body, if any, would begin). out_req is filled in.
 * Does not look at or consume a request body. */
http_parse_result_t http_parse_request(const char *buf, size_t len,
                                        http_request_t *out_req,
                                        size_t *out_consumed);

/* Case-insensitive header lookup. Returns NULL if not present. */
const char *http_get_header(const http_request_t *req, const char *name);

#endif /* HTTP_PARSER_H */
