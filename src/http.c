#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

// Garantiza el envío completo del buffer aunque send() escriba parcialmente
int http_send_all(int fd, const void *data, size_t length) {
    const char *cursor = data;
    while (length > 0) {
        ssize_t sent = send(fd, cursor, length, MSG_NOSIGNAL);
        if (sent < 0) { if (errno == EINTR) continue; return -1; }
        if (sent == 0) return -1;
        cursor += sent;
        length -= (size_t)sent;
    }
    return 0;
}

// Frase estándar asociada a cada código de estado HTTP
const char *http_reason_phrase(int status) {
    switch (status) {
        case 200: return "OK"; case 400: return "Bad Request";
        case 403: return "Forbidden"; case 404: return "Not Found";
        case 405: return "Method Not Allowed"; case 408: return "Request Timeout";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

// Verifica que no haya espacios ni caracteres de control en tokens de la solicitud
static int is_valid_token_chars(const char *start, size_t len) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (c <= 0x20 || c == 0x7F) return 0;
    }
    return 1;
}

// Valida el formato "HTTP/<dígito>.<dígito>"
static int is_http_version_syntax(const char *version, size_t len) {
    if (len < 8 || strncmp(version, "HTTP/", 5) != 0) return 0;
    const char *dot = memchr(version + 5, '.', len - 5);
    if (!dot || dot == version + 5 || dot == version + len - 1) return 0;
    for (const char *p = version + 5; p < dot; p++)
        if (!isdigit((unsigned char)*p)) return 0;
    for (const char *p = dot + 1; p < version + len; p++)
        if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

// Parsea la línea de solicitud y valida la sintaxis de todas las cabeceras
int http_parse_request(const char *raw, size_t length, http_request_t *request) {
    if (!raw || !request || length == 0) return 400;

    const char *line_end = NULL;
    for (size_t i = 0; i + 1 < length; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n') {
            line_end = &raw[i];
            break;
        }
        if (raw[i] == '\r' || raw[i] == '\n') return 400;
    }
    if (!line_end) return 400;

    size_t line_length = (size_t)(line_end - raw);
    if (line_length >= HTTP_REQUEST_MAX) return 400;

    const char *sp1 = memchr(raw, ' ', line_length);
    if (!sp1) return 400;
    size_t rem_after_sp1 = (size_t)(line_end - (sp1 + 1));
    const char *sp2 = memchr(sp1 + 1, ' ', rem_after_sp1);
    if (!sp2) return 400;
    size_t rem_after_sp2 = (size_t)(line_end - (sp2 + 1));
    if (memchr(sp2 + 1, ' ', rem_after_sp2) != NULL) return 400;

    size_t method_len = (size_t)(sp1 - raw);
    size_t target_len = (size_t)(sp2 - (sp1 + 1));
    size_t version_len = rem_after_sp2;

    if (!is_valid_token_chars(raw, method_len) ||
        !is_valid_token_chars(sp1 + 1, target_len) ||
        !is_valid_token_chars(sp2 + 1, version_len)) {
        return 400;
    }

    if (method_len >= HTTP_METHOD_MAX ||
        target_len >= HTTP_TARGET_MAX ||
        version_len >= HTTP_VERSION_MAX) {
        return 400;
    }

    memcpy(request->method, raw, method_len);
    request->method[method_len] = '\0';
    memcpy(request->target, sp1 + 1, target_len);
    request->target[target_len] = '\0';
    memcpy(request->version, sp2 + 1, version_len);
    request->version[version_len] = '\0';

    if (request->target[0] != '/') return 400;
    if (!is_http_version_syntax(request->version, version_len)) return 400;
    if (strcmp(request->version, "HTTP/1.1") != 0) {
        return 505;
    }

    // Validar sintaxis de todas las líneas de cabecera hasta la línea vacía
    const char *cursor = line_end + 2;
    const char *raw_end = raw + length;
    int found_empty_line = 0;
    while (cursor < raw_end) {
        const char *hdr_end = NULL;
        for (const char *p = cursor; p + 1 < raw_end; p++) {
            if (p[0] == '\r' && p[1] == '\n') {
                hdr_end = p;
                break;
            }
            if (p[0] == '\r' || p[0] == '\n') return 400;
        }
        if (!hdr_end) return 400;
        if (hdr_end == cursor) {
            found_empty_line = 1;
            break;
        }

        const char *colon = memchr(cursor, ':', (size_t)(hdr_end - cursor));
        if (!colon) return 400;
        size_t name_len = (size_t)(colon - cursor);
        if (!is_valid_token_chars(cursor, name_len)) return 400;

        for (const char *v = colon + 1; v < hdr_end; v++) {
            unsigned char c = (unsigned char)*v;
            if ((c < 0x20 && c != '\t') || c == 0x7F) return 400;
        }
        cursor = hdr_end + 2;
    }
    if (!found_empty_line) return 400;

    request->keep_alive = 1;
    return 0;
}

// Busca una cabecera por nombre (case-insensitive) y copia su valor sin espacios extremos
int http_get_header(const char *raw, const char *name, char *value, size_t value_size) {
    if (!raw || !name) return 0;
    const char *line = strstr(raw, "\r\n");
    if (!line) return 0;
    line += 2;
    size_t name_len = strlen(name);

    while (*line) {
        const char *end = strstr(line, "\r\n");
        if (!end || end == line) break;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon) {
            size_t len = (size_t)(colon - line);
            while (len > 0 && isspace((unsigned char)line[len - 1])) len--;
            if (name_len == len && strncasecmp(line, name, len) == 0) {
                const char *val_start = colon + 1;
                const char *val_end = end;
                while (val_start < val_end && (*val_start == ' ' || *val_start == '\t')) val_start++;
                while (val_end > val_start && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
                size_t val_len = (size_t)(val_end - val_start);
                if (val_len == 0) return 0;
                if (value && value_size > 0) {
                    size_t copy_len = val_len < value_size - 1 ? val_len : value_size - 1;
                    memcpy(value, val_start, copy_len);
                    value[copy_len] = '\0';
                }
                return 1;
            }
        }
        line = end + 2;
    }
    return 0;
}

int http_has_header(const char *raw, const char *name) {
    return http_get_header(raw, name, NULL, 0);
}

// Arma las cabeceras de respuesta HTTP/1.1 con fecha GMT, longitud y política de conexión
int http_format_headers(char *out, size_t out_size, int status,
                        const char *type, off_t length,
                        int keep_alive, const char *extra) {
    char date[128];
    time_t now = time(NULL);
    struct tm utc;
    if (!gmtime_r(&now, &utc) || !strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc)) {
        snprintf(date, sizeof(date), "Thu, 01 Jan 1970 00:00:00 GMT");
    }
    int n = snprintf(out, out_size,
        "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: %s\r\n%s%s\r\n",
        status, http_reason_phrase(status), date, MINIHTTPD_SERVER_NAME, type,
        (long long)length, keep_alive ? "keep-alive" : "close",
        keep_alive ? "Keep-Alive: timeout=15, max=100\r\n" : "", extra ? extra : "");
    if (n < 0 || (size_t)n >= out_size) return -1;
    return n;
}

// Genera una respuesta HTML completa para códigos de error (4xx/5xx)
int http_format_error(char *out, size_t out_size, int status,
                      const char *detail, int keep_alive, const char *extra) {
    char body[1200];
    int body_len = snprintf(body, sizeof(body),
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>%d %s</title></head><body><h1>%d %s</h1><p>%s</p></body></html>\n",
        status, http_reason_phrase(status), status, http_reason_phrase(status), detail ? detail : "");
    if (body_len < 0 || (size_t)body_len >= sizeof(body)) return -1;
    int hdr_len = http_format_headers(out, out_size, status, "text/html; charset=utf-8", body_len, keep_alive, extra);
    if (hdr_len < 0 || (size_t)hdr_len + (size_t)body_len >= out_size) return -1;
    memcpy(out + hdr_len, body, (size_t)body_len);
    out[hdr_len + body_len] = '\0';
    return hdr_len + body_len;
}

int http_send_headers(int fd, int status, const char *type, off_t length,
                      int keep_alive, const char *extra) {
    char headers[1200];
    int n = http_format_headers(headers, sizeof(headers), status, type, length, keep_alive, extra);
    if (n < 0) return -1;
    return http_send_all(fd, headers, (size_t)n);
}

int http_send_error(int fd, int status, const char *detail, int keep_alive, const char *extra) {
    char response[2400];
    int n = http_format_error(response, sizeof(response), status, detail, keep_alive, extra);
    if (n < 0) return -1;
    return http_send_all(fd, response, (size_t)n);
}
