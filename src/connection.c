#include "connection.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"
#include "http_parser.h"
#include "http_response.h"
#include "metrics.h"
#include "mime.h"

static void log_state(const server_config_t *cfg, const char *state, const char *detail) {
    if (cfg->verbose) {
        fprintf(stderr, "[conn] %-8s %s\n", state, detail ? detail : "");
    }
}

/* Apache combined-log-ish access line, written to stdout for every
 * completed request regardless of -v: "<ip> - - [date] \"M path ver\" code len" */
static void log_access(const connection_t *conn, const char *method, const char *path,
                        const char *version, int status, long content_length) {
    char date_buf[32];
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(date_buf, sizeof(date_buf), "%d/%b/%Y:%H:%M:%S +0000", &tm_utc);
    printf("%s - - [%s] \"%s %s %s\" %d %ld\n",
           conn->client_ip[0] != '\0' ? conn->client_ip : "-", date_buf, method, path, version,
           status, content_length);
    metrics_on_request(content_length);
}

connection_t *connection_create(int fd, const char *client_ip) {
    connection_t *conn = calloc(1, sizeof(*conn));
    if (conn == NULL) return NULL;
    conn->fd = fd;
    conn->state = CONN_STATE_READING;
    conn->file_fd = -1;
    conn->last_active = time(NULL);
    if (client_ip != NULL) {
        strncpy(conn->client_ip, client_ip, sizeof(conn->client_ip) - 1);
    }
    return conn;
}

void connection_destroy(connection_t *conn) {
    if (conn == NULL) return;
    if (conn->file_fd >= 0) close(conn->file_fd);
    close(conn->fd);
    free(conn);
}

void connection_reset_for_next_request(connection_t *conn) {
    conn->state = CONN_STATE_READING;
    conn->rlen = 0;
    conn->wlen = 0;
    conn->wsent = 0;
    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    conn->file_remaining = 0;
    conn->keep_alive = 0;
    conn->is_head = 0;
    conn->is_sse = 0;
}

/* Builds an error response (headers + optional plain-text body) into
 * conn->wbuf and switches the connection into the writing state. Does not
 * log -- callers log with whatever extra detail (path, method, ...) they
 * have on hand. Returns the body length actually included, for the access
 * log's byte count. */
static int build_error_response(connection_t *conn, int status_code, int include_body,
                                 int keep_alive) {
    char body[256];
    int body_len = include_body ? http_build_error_body(body, sizeof(body), status_code) : 0;
    if (body_len < 0) body_len = 0;

    int header_len = http_build_headers(conn->wbuf, sizeof(conn->wbuf), status_code,
                                         "text/plain; charset=utf-8", body_len, keep_alive);
    if (header_len < 0) {
        /* Can't happen with this fixed small header set, but never write
         * past the buffer if it somehow did. */
        header_len = 0;
        keep_alive = 0;
    }

    conn->wlen = (size_t)header_len;
    if (body_len > 0 && conn->wlen + (size_t)body_len <= sizeof(conn->wbuf)) {
        memcpy(conn->wbuf + conn->wlen, body, (size_t)body_len);
        conn->wlen += (size_t)body_len;
    }
    conn->wsent = 0;
    conn->keep_alive = keep_alive;
    conn->state = CONN_STATE_WRITING;
    return body_len;
}

/* A fully parsed request is available; resolve it and build the response
 * (headers always go in conn->wbuf, a file body if any is streamed later
 * straight from conn->file_fd via sendfile()). */
static void handle_parsed_request(connection_t *conn, const server_config_t *cfg,
                                   const http_request_t *req) {
    int is_head = strcmp(req->method, "HEAD") == 0;
    int is_get = strcmp(req->method, "GET") == 0;
    conn->is_head = is_head;

    /* HTTP/1.0 connections are closed by default unless the client opts
     * into "Connection: keep-alive"; HTTP/1.1 connections stay open unless
     * the client sends "Connection: close". */
    const char *conn_header = http_get_header(req, "Connection");
    int is_http10 = strcmp(req->version, "HTTP/1.0") == 0;
    int keep_alive = is_http10 ? (conn_header != NULL && strcasecmp(conn_header, "keep-alive") == 0)
                                : (conn_header == NULL || strcasecmp(conn_header, "close") != 0);

    if (!is_get && !is_head) {
        log_state(cfg, "405", req->method);
        int body_len = build_error_response(conn, 405, 1, keep_alive);
        log_access(conn, req->method, req->path, req->version, 405, body_len);
        return;
    }

    if (strcmp(req->path, "/metrics") == 0) {
        char json[512];
        int json_len = metrics_format_json(json, sizeof(json));
        if (json_len < 0) json_len = 0;
        int header_len = http_build_headers(conn->wbuf, sizeof(conn->wbuf), 200,
                                             "application/json", json_len, keep_alive);
        if (header_len < 0) {
            int body_len = build_error_response(conn, 500, 1, 0);
            log_access(conn, req->method, req->path, req->version, 500, body_len);
            return;
        }
        conn->wlen = (size_t)header_len;
        if (!is_head && json_len > 0 && conn->wlen + (size_t)json_len <= sizeof(conn->wbuf)) {
            memcpy(conn->wbuf + conn->wlen, json, (size_t)json_len);
            conn->wlen += (size_t)json_len;
        }
        conn->wsent = 0;
        conn->keep_alive = keep_alive;
        conn->state = CONN_STATE_WRITING;
        log_access(conn, req->method, req->path, req->version, 200, json_len);
        return;
    }

    if (is_get && strcmp(req->path, "/metrics/stream") == 0) {
        int header_len = http_build_sse_headers(conn->wbuf, sizeof(conn->wbuf));
        if (header_len < 0) {
            int body_len = build_error_response(conn, 500, 1, 0);
            log_access(conn, req->method, req->path, req->version, 500, body_len);
            return;
        }
        conn->wlen = (size_t)header_len;
        conn->wsent = 0;
        conn->is_sse = 1;
        conn->keep_alive = 1;
        conn->state = CONN_STATE_WRITING;
        log_access(conn, req->method, req->path, req->version, 200, 0);
        return;
    }

    /* /dashboard is just an alias for the static dashboard.html shipped in
     * the document root -- no special-casing needed past the path swap. */
    const char *serve_path = strcmp(req->path, "/dashboard") == 0 ? "/dashboard.html" : req->path;

    char fullpath[4096];
    struct stat st;
    file_resolve_result_t resolved =
        resolve_path(cfg->doc_root, serve_path, fullpath, sizeof(fullpath), &st);

    switch (resolved) {
        case FILE_RESOLVE_OK: {
            log_state(cfg, "200", fullpath);
            int header_len = http_build_headers(conn->wbuf, sizeof(conn->wbuf), 200,
                                                  mime_type_for_path(fullpath), (long)st.st_size,
                                                  keep_alive);
            if (header_len < 0) {
                int body_len = build_error_response(conn, 500, 1, 0);
                log_access(conn, req->method, req->path, req->version, 500, body_len);
                return;
            }
            conn->wlen = (size_t)header_len;
            conn->wsent = 0;
            conn->keep_alive = keep_alive;

            if (!is_head && st.st_size > 0) {
                int file_fd = open(fullpath, O_RDONLY);
                if (file_fd < 0) {
                    int body_len = build_error_response(conn, 500, 1, 0);
                    log_access(conn, req->method, req->path, req->version, 500, body_len);
                    return;
                }
                conn->file_fd = file_fd;
                conn->file_remaining = st.st_size;
            }
            conn->state = CONN_STATE_WRITING;
            log_access(conn, req->method, req->path, req->version, 200, (long)st.st_size);
            break;
        }
        case FILE_RESOLVE_NOT_FOUND: {
            log_state(cfg, "404", req->path);
            int body_len = build_error_response(conn, 404, !is_head, keep_alive);
            log_access(conn, req->method, req->path, req->version, 404, body_len);
            break;
        }
        case FILE_RESOLVE_FORBIDDEN: {
            log_state(cfg, "403", req->path);
            int body_len = build_error_response(conn, 403, !is_head, keep_alive);
            log_access(conn, req->method, req->path, req->version, 403, body_len);
            break;
        }
        case FILE_RESOLVE_BAD_REQUEST:
        default: {
            log_state(cfg, "400", req->path);
            int body_len = build_error_response(conn, 400, !is_head, keep_alive);
            log_access(conn, req->method, req->path, req->version, 400, body_len);
            break;
        }
    }
}

conn_io_result_t connection_on_readable(connection_t *conn, const server_config_t *cfg) {
    for (;;) {
        if (conn->rlen >= sizeof(conn->rbuf) - 1) {
            /* Defensive only: http_parse_request already reports
             * HTTP_PARSE_TOO_LARGE once the unparsed header section reaches
             * CONN_READ_BUF_SIZE, so this should never trigger first. */
            log_state(cfg, "400", "request too large");
            int body_len = build_error_response(conn, 400, 1, 0);
            log_access(conn, "-", "-", "-", 400, body_len);
            return CONN_IO_WANT_WRITE;
        }

        ssize_t n = read(conn->fd, conn->rbuf + conn->rlen, sizeof(conn->rbuf) - 1 - conn->rlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return CONN_IO_CLOSE;
        }
        if (n == 0) {
            /* Peer closed. Clean between requests, or mid-request -- either
             * way there's nothing left to read or respond to. */
            return CONN_IO_CLOSE;
        }

        conn->rlen += (size_t)n;
        conn->rbuf[conn->rlen] = '\0';
        conn->last_active = time(NULL);

        http_request_t req;
        size_t consumed = 0;
        http_parse_result_t pr = http_parse_request(conn->rbuf, conn->rlen, &req, &consumed);

        if (pr == HTTP_PARSE_INCOMPLETE) {
            continue; /* level-triggered epoll will re-fire if more data is
                         pending; if not, we loop back to read() and get
                         EAGAIN, breaking out to wait for the next event */
        }
        if (pr == HTTP_PARSE_OK) {
            log_state(cfg, "parsed", req.path);
            handle_parsed_request(conn, cfg, &req);
            /* Bytes after the parsed head (pipelined requests) are
             * discarded -- this server does not support pipelining. */
            return CONN_IO_WANT_WRITE;
        }
        /* HTTP_PARSE_TOO_LARGE or HTTP_PARSE_ERROR: framing is unknown at
         * this point, so the connection cannot be kept alive. */
        log_state(cfg, "400", pr == HTTP_PARSE_TOO_LARGE ? "request too large" : "malformed request");
        int body_len = build_error_response(conn, 400, 1, 0);
        log_access(conn, "-", "-", "-", 400, body_len);
        return CONN_IO_WANT_WRITE;
    }
    return CONN_IO_AGAIN;
}

conn_io_result_t connection_on_writable(connection_t *conn, const server_config_t *cfg) {
    (void)cfg;

    while (conn->wsent < conn->wlen) {
        ssize_t n = write(conn->fd, conn->wbuf + conn->wsent, conn->wlen - conn->wsent);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return CONN_IO_AGAIN;
            return CONN_IO_CLOSE;
        }
        conn->wsent += (size_t)n;
        conn->last_active = time(NULL);
    }

    while (conn->file_fd >= 0 && conn->file_remaining > 0) {
        ssize_t n = sendfile(conn->fd, conn->file_fd, NULL, (size_t)conn->file_remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return CONN_IO_AGAIN;
            return CONN_IO_CLOSE;
        }
        if (n == 0) {
            /* File shrank underneath us since stat(); nothing more to send. */
            break;
        }
        conn->file_remaining -= n;
        conn->last_active = time(NULL);
    }

    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    if (conn->is_sse) {
        /* Stays open indefinitely; the next frame is pushed by the sweep
         * tick in main.c, not by another epoll-driven write. */
        return CONN_IO_SSE_ACTIVE;
    }
    return conn->keep_alive ? CONN_IO_WANT_READ : CONN_IO_CLOSE;
}

conn_io_result_t connection_sse_push(connection_t *conn, const server_config_t *cfg,
                                      const char *json, size_t json_len) {
    int n = snprintf(conn->wbuf, sizeof(conn->wbuf), "data: %.*s\n\n", (int)json_len, json);
    if (n < 0 || (size_t)n >= sizeof(conn->wbuf)) {
        return CONN_IO_AGAIN; /* drop this frame; try again on the next tick */
    }
    conn->wlen = (size_t)n;
    conn->wsent = 0;
    return connection_on_writable(conn, cfg);
}
