#include "connection.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"
#include "http_parser.h"
#include "http_response.h"
#include "mime.h"

#define REQUEST_BUF_SIZE (16 * 1024)

static void log_state(const server_config_t *cfg, const char *state, const char *detail) {
    if (cfg->verbose) {
        fprintf(stderr, "[conn] %-8s %s\n", state, detail ? detail : "");
    }
}

/* Reads from client_fd until a full request head (request line + headers +
 * blank line) has arrived, the buffer fills up, or the connection closes.
 * Returns the parse result; on HTTP_PARSE_OK, *req is populated. */
static http_parse_result_t read_request(int client_fd, const server_config_t *cfg,
                                         char *buf, size_t buf_size,
                                         http_request_t *req) {
    size_t total = 0;
    for (;;) {
        size_t consumed = 0;
        http_parse_result_t result = http_parse_request(buf, total, req, &consumed);
        if (result == HTTP_PARSE_OK || result == HTTP_PARSE_ERROR ||
            result == HTTP_PARSE_TOO_LARGE) {
            return result;
        }
        /* HTTP_PARSE_INCOMPLETE: need more bytes */
        if (total >= buf_size - 1) {
            return HTTP_PARSE_TOO_LARGE;
        }

        ssize_t n = read(client_fd, buf + total, buf_size - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_state(cfg, "read-err", strerror(errno));
            return HTTP_PARSE_ERROR;
        }
        if (n == 0) {
            /* client closed before sending a full request head */
            return HTTP_PARSE_ERROR;
        }
        total += (size_t)n;
        buf[total] = '\0';
    }
}

static void handle_request(int client_fd, const server_config_t *cfg, const http_request_t *req) {
    int is_head = strcmp(req->method, "HEAD") == 0;
    int is_get = strcmp(req->method, "GET") == 0;

    if (!is_get && !is_head) {
        log_state(cfg, "405", req->method);
        http_send_error_response(client_fd, 405, 1, 0);
        return;
    }

    char fullpath[4096];
    struct stat st;
    file_resolve_result_t resolved =
        resolve_path(cfg->doc_root, req->path, fullpath, sizeof(fullpath), &st);

    switch (resolved) {
        case FILE_RESOLVE_OK:
            log_state(cfg, "200", fullpath);
            http_send_file_response(client_fd, fullpath, &st, mime_type_for_path(fullpath),
                                     !is_head, 0);
            break;
        case FILE_RESOLVE_NOT_FOUND:
            log_state(cfg, "404", req->path);
            http_send_error_response(client_fd, 404, !is_head, 0);
            break;
        case FILE_RESOLVE_FORBIDDEN:
            log_state(cfg, "403", req->path);
            http_send_error_response(client_fd, 403, !is_head, 0);
            break;
        case FILE_RESOLVE_BAD_REQUEST:
        default:
            log_state(cfg, "400", req->path);
            http_send_error_response(client_fd, 400, !is_head, 0);
            break;
    }
}

void handle_connection(int client_fd, const server_config_t *cfg) {
    char buf[REQUEST_BUF_SIZE];
    http_request_t req;

    log_state(cfg, "accept", NULL);
    http_parse_result_t result = read_request(client_fd, cfg, buf, sizeof(buf), &req);

    switch (result) {
        case HTTP_PARSE_OK:
            log_state(cfg, "parsed", req.path);
            handle_request(client_fd, cfg, &req);
            break;
        case HTTP_PARSE_TOO_LARGE:
            log_state(cfg, "400", "request too large");
            http_send_error_response(client_fd, 400, 1, 0);
            break;
        case HTTP_PARSE_ERROR:
        case HTTP_PARSE_INCOMPLETE: /* unreachable: read_request loops until non-incomplete */
        default:
            log_state(cfg, "400", "malformed request");
            http_send_error_response(client_fd, 400, 1, 0);
            break;
    }

    log_state(cfg, "close", NULL);
    close(client_fd);
}
