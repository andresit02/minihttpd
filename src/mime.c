#include "mime.h"
#include <string.h>
#include <strings.h>

typedef struct { const char *extension; const char *type; } mime_entry_t;
static const mime_entry_t entries[] = {
    {".html", "text/html"}, {".htm", "text/html"},
    {".css", "text/css"}, {".js", "application/javascript"},
    {".json", "application/json"}, {".txt", "text/plain"},
    {".xml", "application/xml"}, {".png", "image/png"},
    {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
    {".svg", "image/svg+xml"}, {".ico", "image/x-icon"}, {".pdf", "application/pdf"},
    {NULL, NULL}
};

const char *mime_type_for_path(const char *path) {
    const char *extension = path ? strrchr(path, '.') : NULL;
    if (!extension) return "application/octet-stream";
    for (size_t i = 0; entries[i].extension; i++)
        if (strcasecmp(extension, entries[i].extension) == 0) return entries[i].type;
    return "application/octet-stream";
}
