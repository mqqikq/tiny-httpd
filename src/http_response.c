#include "http_response.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "tiny-httpd/0.1"
#define IO_BUF_SIZE (64 * 1024)

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

/* Writes the full buffer, looping over short writes and retrying on EINTR.
 * Returns 0 on success, -1 if the connection broke before all bytes went out. */
static int write_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int send_headers(int fd, int status_code, const char *content_type,
                         long content_length, int keep_alive) {
    char date_buf[64];
    format_http_date(date_buf, sizeof(date_buf));

    char header[1024];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Date: %s\r\n"
                      "Server: " SERVER_NAME "\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %ld\r\n"
                      "Connection: %s\r\n"
                      "\r\n",
                      status_code, http_status_text(status_code), date_buf,
                      content_type, content_length, keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= sizeof(header)) {
        return -1;
    }
    return write_all(fd, header, (size_t)n);
}

int http_send_error_response(int fd, int status_code, int include_body, int keep_alive) {
    char body[256];
    int body_len = snprintf(body, sizeof(body), "%d %s\n", status_code,
                             http_status_text(status_code));
    if (body_len < 0) body_len = 0;

    if (send_headers(fd, status_code, "text/plain; charset=utf-8", body_len, keep_alive) != 0) {
        return -1;
    }
    if (include_body && write_all(fd, body, (size_t)body_len) != 0) {
        return -1;
    }
    return 0;
}

int http_send_file_response(int fd, const char *filepath, const struct stat *st,
                             const char *mime_type, int include_body, int keep_alive) {
    if (send_headers(fd, 200, mime_type, (long)st->st_size, keep_alive) != 0) {
        return -1;
    }
    if (!include_body) {
        return 0;
    }

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }

    char buf[IO_BUF_SIZE];
    int result = 0;
    for (;;) {
        ssize_t n = read(file_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            result = -1;
            break;
        }
        if (n == 0) break;
        if (write_all(fd, buf, (size_t)n) != 0) {
            result = -1;
            break;
        }
    }
    close(file_fd);
    return result;
}
