#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection.h"
#include "server.h"

#define DEFAULT_PORT 8080
#define DEFAULT_ROOT "www"
#define DEFAULT_BACKLOG 128

static volatile sig_atomic_t g_shutdown = 0;

static void on_sigint(int signum) {
    (void)signum;
    g_shutdown = 1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-p port] [-r doc_root]\n"
            "  -p, --port      TCP port to listen on (default %d)\n"
            "  -r, --root      directory to serve files from (default %s)\n"
            "  -v, --verbose   log connection state transitions to stderr\n"
            "  -h, --help      show this help\n",
            prog, DEFAULT_PORT, DEFAULT_ROOT);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    const char *root_arg = DEFAULT_ROOT;
    int verbose = 0;

    static struct option long_opts[] = {
        {"port", required_argument, NULL, 'p'},
        {"root", required_argument, NULL, 'r'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:r:vh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'r':
                root_arg = optarg;
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

    char doc_root[PATH_MAX];
    if (realpath(root_arg, doc_root) == NULL) {
        fprintf(stderr, "error: cannot resolve document root '%s': %s\n", root_arg,
                strerror(errno));
        return 1;
    }

    /* Writing to a socket the peer already closed must not kill the
     * process -- handle the resulting EPIPE as a normal write error. */
    signal(SIGPIPE, SIG_IGN);

    /* glibc's signal() installs handlers with SA_RESTART, which would make
     * the blocking accept() below silently resume after SIGINT/SIGTERM
     * instead of returning -1/EINTR -- so the shutdown flag would never get
     * checked. sigaction() without SA_RESTART is what actually makes
     * graceful shutdown work. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int listen_fd = server_listen(port, DEFAULT_BACKLOG);
    if (listen_fd < 0) {
        return 1;
    }

    server_config_t cfg = {.doc_root = doc_root, .verbose = verbose};
    printf("listening on port %d, serving %s\n", port, doc_root);

    while (!g_shutdown) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_connection(client_fd, &cfg);
    }

    printf("\nshutting down\n");
    close(listen_fd);
    return 0;
}
