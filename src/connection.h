#ifndef CONNECTION_H
#define CONNECTION_H

typedef struct {
    const char *doc_root; /* canonical, absolute, no trailing slash */
    int verbose;          /* log per-request state transitions to stderr */
} server_config_t;

/* Reads one HTTP request off client_fd, serves it (static file or error),
 * and closes client_fd before returning. Blocking I/O; one request per
 * connection (Connection: close always) -- this is the week-1 baseline,
 * superseded by the non-blocking keep-alive event loop. */
void handle_connection(int client_fd, const server_config_t *cfg);

#endif /* CONNECTION_H */
