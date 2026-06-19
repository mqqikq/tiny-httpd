#include "metrics.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

static struct {
    time_t start_time;
    long connections_total;
    long connections_active;
    long requests_total;
    long long bytes_sent_total;
} g_metrics;

void metrics_init(void) {
    g_metrics.start_time = time(NULL);
    g_metrics.connections_total = 0;
    g_metrics.connections_active = 0;
    g_metrics.requests_total = 0;
    g_metrics.bytes_sent_total = 0;
}

void metrics_on_connection_open(void) {
    g_metrics.connections_total++;
    g_metrics.connections_active++;
}

void metrics_on_connection_close(void) {
    if (g_metrics.connections_active > 0) g_metrics.connections_active--;
}

void metrics_on_request(long bytes_sent) {
    g_metrics.requests_total++;
    if (bytes_sent > 0) g_metrics.bytes_sent_total += bytes_sent;
}

int metrics_format_json(char *out, size_t out_size) {
    time_t uptime = time(NULL) - g_metrics.start_time;
    if (uptime < 1) uptime = 1;
    double req_per_sec = (double)g_metrics.requests_total / (double)uptime;

    int n = snprintf(out, out_size,
                      "{"
                      "\"pid\":%d,"
                      "\"uptime_sec\":%ld,"
                      "\"connections_active\":%ld,"
                      "\"connections_total\":%ld,"
                      "\"requests_total\":%ld,"
                      "\"bytes_sent_total\":%lld,"
                      "\"requests_per_sec_avg\":%.2f"
                      "}",
                      (int)getpid(), (long)uptime, g_metrics.connections_active,
                      g_metrics.connections_total, g_metrics.requests_total,
                      g_metrics.bytes_sent_total, req_per_sec);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return n;
}
