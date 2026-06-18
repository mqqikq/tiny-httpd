#ifndef CONNECTION_H
#define CONNECTION_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define CONN_READ_BUF_SIZE  (16 * 1024)
#define CONN_WRITE_BUF_SIZE (8 * 1024)
#define CONN_IP_STRLEN      16 /* "255.255.255.255\0", IPv4 only */

typedef struct {
    const char *doc_root;     /* canonical, absolute, no trailing slash */
    int verbose;               /* log per-request state transitions to stderr */
    int idle_timeout_sec;      /* close a connection idle this long, even keep-alive ones */
} server_config_t;

typedef enum {
    CONN_STATE_READING, /* accumulating bytes, no full request head yet */
    CONN_STATE_WRITING, /* response built, draining wbuf and/or streaming file_fd */
} conn_state_t;

/* One TCP connection's state, driven entirely by epoll readiness -- never
 * blocks. A single connection_t is reused across keep-alive requests on the
 * same fd via connection_reset_for_next_request(). */
typedef struct connection {
    int fd;
    char client_ip[CONN_IP_STRLEN];
    conn_state_t state;

    char rbuf[CONN_READ_BUF_SIZE];
    size_t rlen;

    char wbuf[CONN_WRITE_BUF_SIZE];
    size_t wlen;
    size_t wsent;

    int file_fd;            /* >= 0 while streaming a file body, else -1 */
    off_t file_remaining;

    int keep_alive;
    int is_head;

    time_t last_active;     /* for idle-timeout sweeps */
} connection_t;

typedef enum {
    CONN_IO_AGAIN,      /* not done; keep waiting on the same epoll interest */
    CONN_IO_WANT_WRITE,  /* request fully read and handled; switch to EPOLLOUT */
    CONN_IO_WANT_READ,   /* response fully sent, keep-alive; switch back to EPOLLIN */
    CONN_IO_CLOSE,       /* done (or unrecoverable error); close the connection */
} conn_io_result_t;

/* client_ip (e.g. "203.0.113.7") is copied in for access-log lines; pass
 * NULL if unavailable (logged as "-"). */
connection_t *connection_create(int fd, const char *client_ip);
void connection_destroy(connection_t *conn);

/* Resets per-request state (read/write buffers, file fd, flags) so the
 * connection can be reused for the next request on a keep-alive fd. */
void connection_reset_for_next_request(connection_t *conn);

/* Call when epoll reports the fd as readable. Reads as much as is currently
 * available (non-blocking), tries to parse a full request, and on success
 * builds the response into the connection. */
conn_io_result_t connection_on_readable(connection_t *conn, const server_config_t *cfg);

/* Call when epoll reports the fd as writable. Drains the header/body buffer
 * and streams any pending file body via sendfile(). */
conn_io_result_t connection_on_writable(connection_t *conn, const server_config_t *cfg);

#endif /* CONNECTION_H */
