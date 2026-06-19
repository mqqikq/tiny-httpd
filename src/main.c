#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "connection.h"
#include "event_loop.h"
#include "metrics.h"
#include "server.h"

#define DEFAULT_PORT 8080
#define DEFAULT_ROOT "www"
#define DEFAULT_BACKLOG 128
#define DEFAULT_IDLE_TIMEOUT_SEC 30
#define DEFAULT_WORKERS 1
#define MAX_EVENTS 256
#define TIMEOUT_SWEEP_MS 1000

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_child_died = 0;

static void on_sigint(int signum) {
    (void)signum;
    g_shutdown = 1;
}

static void on_sigchld(int signum) {
    (void)signum;
    g_child_died = 1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-p port] [-r doc_root] [-t idle_timeout] [-w workers]\n"
            "  -p, --port      TCP port to listen on (default %d)\n"
            "  -r, --root      directory to serve files from (default %s)\n"
            "  -t, --timeout   idle keep-alive timeout in seconds (default %d)\n"
            "  -w, --workers   worker processes sharing the port via SO_REUSEPORT (default %d)\n"
            "  -v, --verbose   log connection state transitions to stderr\n"
            "  -h, --help      show this help\n",
            prog, DEFAULT_PORT, DEFAULT_ROOT, DEFAULT_IDLE_TIMEOUT_SEC, DEFAULT_WORKERS);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Closes and forgets a connection at table slot fd. */
static void drop_connection(event_loop_t *loop, connection_t **conns, int fd) {
    event_loop_del(loop, fd);
    connection_destroy(conns[fd]);
    conns[fd] = NULL;
    metrics_on_connection_close();
}

/* Binds its own listening socket (SO_REUSEPORT lets several of these run on
 * the same port at once) and serves requests until g_shutdown is set.
 * This is the entire single-process server; -w workers just runs several
 * of these in parallel via fork(). */
static int run_worker(int port, const char *doc_root, int verbose, int idle_timeout_sec) {
    /* stdout is fully buffered (not line-buffered) when it isn't a TTY --
     * e.g. under `docker logs` or any redirect -- which would otherwise
     * delay the startup banner and every access-log line indefinitely. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    metrics_init();

    int listen_fd = server_listen(port, DEFAULT_BACKLOG);
    if (listen_fd < 0) {
        return 1;
    }
    if (set_nonblocking(listen_fd) < 0) {
        perror("fcntl(O_NONBLOCK) on listen socket");
        close(listen_fd);
        return 1;
    }

    event_loop_t *loop = event_loop_create();
    if (loop == NULL) {
        close(listen_fd);
        return 1;
    }
    if (event_loop_add(loop, listen_fd, EPOLLIN) < 0) {
        perror("event_loop_add(listen_fd)");
        close(listen_fd);
        event_loop_destroy(loop);
        return 1;
    }

    /* Connections are indexed directly by fd (small non-negative integers
     * handed out by the kernel), so the table only needs to be as large as
     * the process's open-file-descriptor limit. */
    struct rlimit rl;
    rlim_t max_fds = 65536;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        max_fds = rl.rlim_cur;
    }
    connection_t **conns = calloc((size_t)max_fds, sizeof(connection_t *));
    if (conns == NULL) {
        fprintf(stderr, "error: out of memory allocating connection table\n");
        close(listen_fd);
        event_loop_destroy(loop);
        return 1;
    }

    server_config_t cfg = {
        .doc_root = doc_root, .verbose = verbose, .idle_timeout_sec = idle_timeout_sec};
    printf("[pid %d] listening on port %d, serving %s (idle timeout %ds)\n", (int)getpid(), port,
           doc_root, idle_timeout_sec);

    struct epoll_event events[MAX_EVENTS];
    time_t last_sweep = time(NULL);

    while (!g_shutdown) {
        int n = event_loop_wait(loop, events, MAX_EVENTS, TIMEOUT_SWEEP_MS);
        if (n < 0) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listen_fd) {
                for (;;) {
                    struct sockaddr_in peer_addr;
                    socklen_t peer_len = sizeof(peer_addr);
                    int client_fd = accept4(listen_fd, (struct sockaddr *)&peer_addr, &peer_len,
                                             SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        /* EMFILE/ENFILE (fd exhaustion) or a per-connection
                         * error (ECONNABORTED, ...): stop accepting for
                         * this round, the listening socket stays armed. */
                        break;
                    }
                    if ((rlim_t)client_fd >= max_fds) {
                        close(client_fd); /* defensive: never index out of bounds */
                        continue;
                    }
                    char ip_buf[CONN_IP_STRLEN] = {0};
                    inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));

                    connection_t *conn = connection_create(client_fd, ip_buf);
                    if (conn == NULL || event_loop_add(loop, client_fd, EPOLLIN) < 0) {
                        if (conn != NULL) connection_destroy(conn);
                        else close(client_fd);
                        continue;
                    }
                    conns[client_fd] = conn;
                    metrics_on_connection_open();
                    if (verbose) fprintf(stderr, "[conn] accept   fd=%d ip=%s\n", client_fd, ip_buf);
                }
                continue;
            }

            connection_t *conn = conns[fd];
            if (conn == NULL) continue; /* stale event for an already-closed fd */

            conn_io_result_t result;
            if (ev & (EPOLLHUP | EPOLLERR)) {
                result = CONN_IO_CLOSE;
            } else if (ev & EPOLLOUT) {
                result = connection_on_writable(conn, &cfg);
            } else {
                result = connection_on_readable(conn, &cfg);
            }

            switch (result) {
                case CONN_IO_AGAIN:
                    break;
                case CONN_IO_WANT_WRITE:
                    if (event_loop_mod(loop, fd, EPOLLOUT) < 0) {
                        drop_connection(loop, conns, fd);
                    }
                    break;
                case CONN_IO_WANT_READ:
                    connection_reset_for_next_request(conn);
                    if (event_loop_mod(loop, fd, EPOLLIN) < 0) {
                        drop_connection(loop, conns, fd);
                    }
                    break;
                case CONN_IO_SSE_ACTIVE:
                    /* Header (or a queued metrics frame) fully drained; go
                     * back to waiting on EPOLLIN so we notice the client
                     * disconnecting. The next push happens on the sweep
                     * tick below, not in response to an epoll event. */
                    if (event_loop_mod(loop, fd, EPOLLIN) < 0) {
                        drop_connection(loop, conns, fd);
                    }
                    break;
                case CONN_IO_CLOSE:
                    drop_connection(loop, conns, fd);
                    break;
            }
        }

        time_t now = time(NULL);
        if (now != last_sweep) {
            last_sweep = now;
            char metrics_json[512];
            int metrics_len = -1; /* computed lazily, only if an SSE client exists */
            for (rlim_t fd2 = 0; fd2 < max_fds; fd2++) {
                connection_t *conn = conns[fd2];
                if (conn == NULL) continue;

                /* SSE connections are exempt from the *protocol* idle-timeout
                 * meaning ("client went quiet") -- a healthy one is never
                 * quiet, it gets a frame every tick. But they still need the
                 * same safety net: connection_on_writable() refreshes
                 * last_active on every successful write, so as long as
                 * pushes keep landing, last_active never goes stale and the
                 * timeout below never fires. If a peer vanishes without
                 * closing (kernel send buffer fills, no ACKs), pushes start
                 * failing/blocking, last_active stops refreshing, and this
                 * same check reclaims it after idle_timeout_sec like any
                 * other dead connection -- instead of leaking it forever. */
                if (conn->is_sse && conn->wsent == conn->wlen) {
                    if (metrics_len < 0) metrics_len = metrics_format_json(metrics_json, sizeof(metrics_json));
                    if (metrics_len > 0) {
                        conn_io_result_t r =
                            connection_sse_push(conn, &cfg, metrics_json, (size_t)metrics_len);
                        if (r == CONN_IO_AGAIN) {
                            if (event_loop_mod(loop, (int)fd2, EPOLLOUT) < 0) {
                                drop_connection(loop, conns, (int)fd2);
                                continue;
                            }
                        } else if (r != CONN_IO_SSE_ACTIVE) {
                            drop_connection(loop, conns, (int)fd2);
                            continue;
                        }
                    }
                }

                if (now - conn->last_active >= cfg.idle_timeout_sec) {
                    if (verbose) fprintf(stderr, "[conn] timeout  fd=%d\n", (int)fd2);
                    drop_connection(loop, conns, (int)fd2);
                }
            }
        }
    }

    printf("[pid %d] shutting down\n", (int)getpid());
    for (rlim_t fd = 0; fd < max_fds; fd++) {
        if (conns[fd] != NULL) {
            connection_destroy(conns[fd]);
        }
    }
    free(conns);
    close(listen_fd);
    event_loop_destroy(loop);
    return 0;
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    const char *root_arg = DEFAULT_ROOT;
    int verbose = 0;
    int idle_timeout_sec = DEFAULT_IDLE_TIMEOUT_SEC;
    int workers = DEFAULT_WORKERS;

    static struct option long_opts[] = {
        {"port", required_argument, NULL, 'p'},
        {"root", required_argument, NULL, 'r'},
        {"timeout", required_argument, NULL, 't'},
        {"workers", required_argument, NULL, 'w'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:r:t:w:vh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'r':
                root_arg = optarg;
                break;
            case 't':
                idle_timeout_sec = atoi(optarg);
                break;
            case 'w':
                workers = atoi(optarg);
                break;
            case 'v':
                verbose = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "error: invalid port %d\n", port);
        return 1;
    }
    if (idle_timeout_sec <= 0) {
        fprintf(stderr, "error: invalid timeout %d\n", idle_timeout_sec);
        return 1;
    }
    if (workers <= 0) {
        fprintf(stderr, "error: invalid worker count %d\n", workers);
        return 1;
    }

    char doc_root[PATH_MAX];
    if (realpath(root_arg, doc_root) == NULL) {
        fprintf(stderr, "error: cannot resolve document root '%s': %s\n", root_arg,
                strerror(errno));
        return 1;
    }

    /* Writing to a socket the peer already closed must not kill the
     * process -- handle the resulting EPIPE as a normal write/sendfile
     * error instead. */
    signal(SIGPIPE, SIG_IGN);

    /* sigaction() without SA_RESTART (unlike glibc's signal()) is what
     * makes graceful shutdown work: epoll_wait()/pause() return -1/EINTR
     * instead of silently resuming, so the loop condition gets re-checked. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (workers == 1) {
        return run_worker(port, doc_root, verbose, idle_timeout_sec);
    }

    /* Multi-worker mode: fork `workers` children, each binding its own
     * socket on the same port via SO_REUSEPORT so the kernel distributes
     * incoming connections across them. This process becomes a pure
     * supervisor -- it serves no traffic itself, just waits for a shutdown
     * signal (or an unexpected child exit) and then forwards SIGTERM to
     * every worker and reaps them. */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = on_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = 0;
    sigaction(SIGCHLD, &sa_chld, NULL);

    pid_t *child_pids = calloc((size_t)workers, sizeof(pid_t));
    if (child_pids == NULL) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    int spawned = 0;
    for (int i = 0; i < workers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            break;
        }
        if (pid == 0) {
            /* Child: behave exactly like a single (-w 1) worker from here. */
            free(child_pids);
            return run_worker(port, doc_root, verbose, idle_timeout_sec);
        }
        child_pids[i] = pid;
        spawned++;
    }

    if (spawned == 0) {
        free(child_pids);
        return 1;
    }
    printf("[supervisor pid %d] spawned %d worker(s) on port %d\n", (int)getpid(), spawned, port);

    while (!g_shutdown && !g_child_died) {
        pause();
    }
    int exit_code = (g_child_died && !g_shutdown) ? 1 : 0;

    for (int i = 0; i < spawned; i++) {
        if (child_pids[i] > 0) kill(child_pids[i], SIGTERM);
    }
    for (int i = 0; i < spawned; i++) {
        if (child_pids[i] > 0) waitpid(child_pids[i], NULL, 0);
    }
    free(child_pids);
    printf("[supervisor pid %d] all workers stopped\n", (int)getpid());
    return exit_code;
}
