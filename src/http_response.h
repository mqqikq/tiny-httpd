#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <sys/stat.h>

/* Maps a status code to its reason phrase, e.g. 404 -> "Not Found".
 * Returns "Unknown" for codes not in the small set this server emits. */
const char *http_status_text(int status_code);

/* Sends a minimal text/plain error response with the given status code.
 * If include_body is 0 (used for HEAD requests), only headers are sent. */
int http_send_error_response(int fd, int status_code, int include_body, int keep_alive);

/* Sends a 200 response streaming the file at filepath (already validated by
 * resolve_path) with the given Content-Type. If include_body is 0 (HEAD),
 * only headers are sent. */
int http_send_file_response(int fd, const char *filepath, const struct stat *st,
                             const char *mime_type, int include_body, int keep_alive);

#endif /* HTTP_RESPONSE_H */
