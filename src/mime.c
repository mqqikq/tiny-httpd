#include "mime.h"

#include <string.h>
#include <strings.h>

typedef struct {
    const char *ext;
    const char *type;
} mime_entry_t;

static const mime_entry_t MIME_TABLE[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm",  "text/html; charset=utf-8"},
    {"css",  "text/css; charset=utf-8"},
    {"js",   "application/javascript; charset=utf-8"},
    {"json", "application/json; charset=utf-8"},
    {"txt",  "text/plain; charset=utf-8"},
    {"xml",  "application/xml; charset=utf-8"},
    {"csv",  "text/csv; charset=utf-8"},
    {"png",  "image/png"},
    {"jpg",  "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif",  "image/gif"},
    {"svg",  "image/svg+xml"},
    {"ico",  "image/x-icon"},
    {"webp", "image/webp"},
    {"pdf",  "application/pdf"},
    {"woff", "font/woff"},
    {"woff2","font/woff2"},
    {"ttf",  "font/ttf"},
    {"mp4",  "video/mp4"},
    {"webm", "video/webm"},
    {"wasm", "application/wasm"},
};

static const size_t MIME_TABLE_LEN = sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]);

const char *mime_type_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL || dot[1] == '\0') {
        return "application/octet-stream";
    }

    const char *ext = dot + 1;
    for (size_t i = 0; i < MIME_TABLE_LEN; i++) {
        if (strcasecmp(ext, MIME_TABLE[i].ext) == 0) {
            return MIME_TABLE[i].type;
        }
    }
    return "application/octet-stream";
}
