#ifndef MINIHTTPD_FILES_H
#define MINIHTTPD_FILES_H

#include <stddef.h>
#include <sys/stat.h>

typedef enum {
    FILE_OK = 200,
    FILE_FORBIDDEN = 403,
    FILE_NOT_FOUND = 404,
    FILE_INTERNAL = 500
} file_status_t;

file_status_t file_open_safe(const char *root, const char *uri,
                             int *fd, struct stat *info,
                             char *resolved_path, size_t path_size);

#endif
