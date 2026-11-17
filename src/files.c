#define _POSIX_C_SOURCE 200809L
#include "files.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int inside_root(const char *root, const char *path) {
    size_t length = strlen(root);
    return strncmp(root, path, length) == 0 && (path[length] == '\0' || path[length] == '/');
}

static file_status_t map_errno(int err) {
    if (err == EACCES || err == EPERM) return FILE_FORBIDDEN;
    if (err == ENOENT || err == ENOTDIR || err == ELOOP || err == ENAMETOOLONG) return FILE_NOT_FOUND;
    return FILE_INTERNAL;
}

static int decode_url(char *path) {
    char *source = path, *destination = path;
    while (*source) {
        if (*source == '%') {
            if (!isxdigit((unsigned char)source[1]) || !isxdigit((unsigned char)source[2])) return -1;
            char hex[3] = {source[1], source[2], '\0'};
            long value = strtol(hex, NULL, 16);
            if (value <= 0x1F || value == 0x7F) return -1;
            *destination++ = (char)value; source += 3;
        } else {
            unsigned char c = (unsigned char)*source;
            if (c <= 0x1F || c == 0x7F) return -1;
            *destination++ = *source++;
        }
    }
    *destination = '\0'; return 0;
}

static int safe_uri(const char *uri) {
    if (!uri || uri[0] != '/') return 0;
    const char *part = uri + 1;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (length == 2 && part[0] == '.' && part[1] == '.') return 0;
        part += length; if (*part == '/') part++;
    }
    return 1;
}

file_status_t file_open_safe(const char *root, const char *uri, int *fd,
                             struct stat *info, char *resolved_path, size_t path_size) {
    if (!root || !uri || !fd || !info || !resolved_path) return FILE_INTERNAL;
    *fd = -1;
    char decoded[PATH_MAX], candidate[PATH_MAX], canonical[PATH_MAX];
    size_t uri_len = strlen(uri);
    if (uri_len >= sizeof(decoded)) return FILE_FORBIDDEN;
    memcpy(decoded, uri, uri_len + 1);
    char *query = strchr(decoded, '?'); if (query) *query = '\0';
    if (decode_url(decoded) < 0 || !safe_uri(decoded)) return FILE_FORBIDDEN;
    int n = snprintf(candidate, sizeof(candidate), "%s%s", root, strcmp(decoded, "/") == 0 ? "/index.html" : decoded);
    if (n < 0 || (size_t)n >= sizeof(candidate)) return FILE_FORBIDDEN;
    if (!realpath(candidate, canonical)) return map_errno(errno);
    if (!inside_root(root, canonical)) return FILE_FORBIDDEN;
    if (stat(canonical, info) < 0) return map_errno(errno);
    if (S_ISDIR(info->st_mode)) {
        n = snprintf(candidate, sizeof(candidate), "%s/index.html", canonical);
        if (n < 0 || (size_t)n >= sizeof(candidate)) return FILE_FORBIDDEN;
        if (!realpath(candidate, canonical)) return map_errno(errno);
        if (!inside_root(root, canonical)) return FILE_FORBIDDEN;
        if (stat(canonical, info) < 0) return map_errno(errno);
    }
    if (!S_ISREG(info->st_mode)) return FILE_FORBIDDEN;
    size_t canonical_len = strlen(canonical);
    if (canonical_len + 1 > path_size) return FILE_INTERNAL;
    memcpy(resolved_path, canonical, canonical_len + 1);
    *fd = open(canonical, O_RDONLY | O_CLOEXEC);
    if (*fd < 0) return map_errno(errno);
    if (fstat(*fd, info) < 0) { close(*fd); *fd = -1; return FILE_INTERNAL; }
    return FILE_OK;
}
