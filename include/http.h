#ifndef MINIHTTPD_HTTP_H
#define MINIHTTPD_HTTP_H

#include <stddef.h>
#include <sys/types.h>

#define HTTP_METHOD_MAX 16
#define HTTP_TARGET_MAX 4096
#define HTTP_VERSION_MAX 16
#define HTTP_REQUEST_MAX 8192

typedef struct {
    char method[HTTP_METHOD_MAX];
    char target[HTTP_TARGET_MAX];
    char version[HTTP_VERSION_MAX];
    int keep_alive;
} http_request_t;

int http_parse_request(const char *raw, size_t length, http_request_t *request);
int http_has_header(const char *raw, const char *name);
int http_get_header(const char *raw, const char *name, char *value, size_t value_size);
int http_send_all(int fd, const void *data, size_t length);
int http_format_headers(char *out, size_t out_size, int status,
                        const char *content_type, off_t content_length,
                        int keep_alive, const char *extra_headers);
int http_format_error(char *out, size_t out_size, int status,
                      const char *detail, int keep_alive,
                      const char *extra_headers);
int http_send_headers(int fd, int status, const char *content_type,
                     off_t content_length, int keep_alive,
                     const char *extra_headers);
int http_send_error(int fd, int status, const char *detail,
                    int keep_alive, const char *extra_headers);
const char *http_reason_phrase(int status);

#endif
