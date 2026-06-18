#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <stdint.h>
#include <sys/epoll.h>

typedef struct {
    int epoll_fd;
} event_loop_t;

/* Creates a new epoll instance. Returns NULL on failure (errno set, a
 * diagnostic has already been printed to stderr). */
event_loop_t *event_loop_create(void);

/* Registers fd for the given epoll event mask (EPOLLIN/EPOLLOUT/...).
 * events.data.fd is set to fd. Returns 0 on success, -1 on failure. */
int event_loop_add(event_loop_t *loop, int fd, uint32_t events);

/* Changes the event mask already registered for fd. Returns 0 on success,
 * -1 on failure (e.g. the peer already closed fd out from under us). */
int event_loop_mod(event_loop_t *loop, int fd, uint32_t events);

/* Unregisters fd. Safe to call even if fd was never added or was already
 * closed -- the resulting ENOENT/EBADF from epoll_ctl is ignored, since the
 * kernel also auto-removes a fd from all epoll sets when it is closed. */
void event_loop_del(event_loop_t *loop, int fd);

/* Blocks for up to timeout_ms (or indefinitely if timeout_ms < 0) and fills
 * out_events (capacity max_events) with ready events. Returns the number of
 * ready events, 0 on timeout, or -1 on a real error (EINTR is retried
 * internally and never returned to the caller). */
int event_loop_wait(event_loop_t *loop, struct epoll_event *out_events, int max_events,
                     int timeout_ms);

void event_loop_destroy(event_loop_t *loop);

#endif /* EVENT_LOOP_H */
