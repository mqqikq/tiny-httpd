#ifndef MIME_H
#define MIME_H

/* Returns a MIME type string for a given file path, based on its extension.
 * Falls back to "application/octet-stream" for unknown extensions.
 * The returned pointer is static and must not be freed. */
const char *mime_type_for_path(const char *path);

#endif /* MIME_H */
