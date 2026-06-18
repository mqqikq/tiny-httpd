/* Unit tests for http_parser.c. No framework: each check prints PASS/FAIL
 * and the process exits non-zero if anything failed, so `make unit` can
 * gate CI. */
#include <stdio.h>
#include <string.h>

#include "http_parser.h"

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

static void test_simple_get(void) {
    printf("test_simple_get\n");
    const char *raw = "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";
    http_request_t req;
    size_t consumed = 0;
    http_parse_result_t r = http_parse_request(raw, strlen(raw), &req, &consumed);

    CHECK(r == HTTP_PARSE_OK, "parse result is OK");
    CHECK(strcmp(req.method, "GET") == 0, "method is GET");
    CHECK(strcmp(req.path, "/index.html") == 0, "path is /index.html");
    CHECK(strcmp(req.version, "HTTP/1.1") == 0, "version is HTTP/1.1");
    CHECK(req.header_count == 1, "one header parsed");
    CHECK(consumed == strlen(raw), "consumed all bytes");

    const char *host = http_get_header(&req, "host");
    CHECK(host != NULL && strcmp(host, "example.com") == 0, "case-insensitive header lookup");
}

static void test_multiple_headers_and_extra_body(void) {
    printf("test_multiple_headers_and_extra_body\n");
    const char *raw =
        "HEAD /a/b.html HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "X-Custom:   value-with-spaces  \r\n"
        "\r\n"
        "this is body data that should not be consumed";
    http_request_t req;
    size_t consumed = 0;
    http_parse_result_t r = http_parse_request(raw, strlen(raw), &req, &consumed);

    CHECK(r == HTTP_PARSE_OK, "parse result is OK");
    CHECK(req.header_count == 3, "three headers parsed");
    const char *custom = http_get_header(&req, "X-Custom");
    CHECK(custom != NULL && strcmp(custom, "value-with-spaces") == 0,
          "header value whitespace trimmed");

    size_t header_section_len = strlen(raw) - strlen("this is body data that should not be consumed");
    CHECK(consumed == header_section_len, "consumed stops before body");
}

static void test_incomplete_request(void) {
    printf("test_incomplete_request\n");
    const char *raw = "GET / HTTP/1.1\r\nHost: x\r\n";
    http_request_t req;
    size_t consumed = 0;
    http_parse_result_t r = http_parse_request(raw, strlen(raw), &req, &consumed);
    CHECK(r == HTTP_PARSE_INCOMPLETE, "missing terminating blank line is INCOMPLETE");
}

static void test_malformed_request_line(void) {
    printf("test_malformed_request_line\n");
    const char *raw = "GET\r\n\r\n";
    http_request_t req;
    size_t consumed = 0;
    http_parse_result_t r = http_parse_request(raw, strlen(raw), &req, &consumed);
    CHECK(r == HTTP_PARSE_ERROR, "request line missing path/version is an ERROR");
}

static void test_bad_version(void) {
    printf("test_bad_version\n");
    const char *raw = "GET / HTTP/2.0\r\n\r\n";
    http_request_t req;
    size_t consumed = 0;
    http_parse_result_t r = http_parse_request(raw, strlen(raw), &req, &consumed);
    CHECK(r == HTTP_PARSE_ERROR, "unsupported HTTP version is an ERROR");
}

static void test_header_without_colon(void) {
    printf("test_header_without_colon\n");
    const char *raw = "GET / HTTP/1.1\r\nBrokenHeader\r\n\r\n";
    http_request_t req;
    size_t consumed = 0;
    http_parse_result_t r = http_parse_request(raw, strlen(raw), &req, &consumed);
    CHECK(r == HTTP_PARSE_ERROR, "header line without a colon is an ERROR");
}

int main(void) {
    test_simple_get();
    test_multiple_headers_and_extra_body();
    test_incomplete_request();
    test_malformed_request_line();
    test_bad_version();
    test_header_without_colon();

    if (g_failures == 0) {
        printf("All http_parser tests passed.\n");
        return 0;
    }
    printf("%d http_parser test(s) FAILED.\n", g_failures);
    return 1;
}
