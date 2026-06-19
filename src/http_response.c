#include "http_response.h"

#include <stdio.h>
#include <time.h>

#define SERVER_NAME "tiny-httpd/0.2"

const char *http_status_text(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 414: return "URI Too Long";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

static void format_http_date(char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(out, out_size, "%a, %d %b %Y %H:%M:%S GMT", &tm_utc);
}

int http_build_headers(char *out, size_t out_size, int status_code, const char *content_type,
                        long content_length, int keep_alive) {
    char date_buf[64];
    format_http_date(date_buf, sizeof(date_buf));

    int n = snprintf(out, out_size,
                      "HTTP/1.1 %d %s\r\n"
                      "Date: %s\r\n"
                      "Server: " SERVER_NAME "\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %ld\r\n"
                      "Connection: %s\r\n"
                      "\r\n",
                      status_code, http_status_text(status_code), date_buf,
                      content_type, content_length, keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    return n;
}

int http_build_error_body(char *out, size_t out_size, int status_code) {
    int n = snprintf(out, out_size, "%d %s\n", status_code, http_status_text(status_code));
    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    return n;
}

int http_build_sse_headers(char *out, size_t out_size) {
    char date_buf[64];
    format_http_date(date_buf, sizeof(date_buf));

    int n = snprintf(out, out_size,
                      "HTTP/1.1 200 OK\r\n"
                      "Date: %s\r\n"
                      "Server: " SERVER_NAME "\r\n"
                      "Content-Type: text/event-stream\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n",
                      date_buf);
    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    return n;
}
