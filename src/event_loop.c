#include "event_loop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

event_loop_t *event_loop_create(void) {
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return NULL;
    }
    event_loop_t *loop = malloc(sizeof(*loop));
    if (loop == NULL) {
        close(epoll_fd);
        return NULL;
    }
    loop->epoll_fd = epoll_fd;
    return loop;
}

static int ctl(event_loop_t *loop, int op, int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(loop->epoll_fd, op, fd, &ev);
}

int event_loop_add(event_loop_t *loop, int fd, uint32_t events) {
    return ctl(loop, EPOLL_CTL_ADD, fd, events);
}

int event_loop_mod(event_loop_t *loop, int fd, uint32_t events) {
    return ctl(loop, EPOLL_CTL_MOD, fd, events);
}

void event_loop_del(event_loop_t *loop, int fd) {
    epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

int event_loop_wait(event_loop_t *loop, struct epoll_event *out_events, int max_events,
                     int timeout_ms) {
    for (;;) {
        int n = epoll_wait(loop->epoll_fd, out_events, max_events, timeout_ms);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

void event_loop_destroy(event_loop_t *loop) {
    if (loop == NULL) return;
    close(loop->epoll_fd);
    free(loop);
}
