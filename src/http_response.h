#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>

/* Maps a status code to its reason phrase, e.g. 404 -> "Not Found".
 * Returns "Unknown" for codes not in the small set this server emits. */
const char *http_status_text(int status_code);

/* Renders "HTTP/1.1 <code> <reason>\r\nDate: ...\r\n...\r\n\r\n" into out
 * (not NUL-terminated; callers track the returned length). Pure formatting,
 * no I/O -- the connection state machine writes the result to the socket
 * itself, possibly across several non-blocking write() calls.
 * Returns the number of bytes written, or -1 if out_size was too small. */
int http_build_headers(char *out, size_t out_size, int status_code,
                        const char *content_type, long content_length, int keep_alive);

/* Renders the small plain-text error body ("404 Not Found\n") into out.
 * Returns the number of bytes written, or -1 if out_size was too small. */
int http_build_error_body(char *out, size_t out_size, int status_code);

#endif /* HTTP_RESPONSE_H */
