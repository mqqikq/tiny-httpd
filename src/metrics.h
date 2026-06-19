#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>

/* Process-wide counters. Each worker process (see -w in main.c) has its own
 * independent copy -- with SO_REUSEPORT the kernel hands each connection to
 * exactly one worker, so there is no cross-process aggregation, and no
 * locking is needed either: every counter is only ever touched from the
 * single event-loop thread that owns it. */
void metrics_init(void);
void metrics_on_connection_open(void);
void metrics_on_connection_close(void);
void metrics_on_request(long bytes_sent);

/* Formats a JSON snapshot into out (caller-sized buffer). Returns the
 * length written, or -1 if it didn't fit. */
int metrics_format_json(char *out, size_t out_size);

#endif /* METRICS_H */
