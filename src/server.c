#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "files.h"
#include "http.h"
#include "mime.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

// Crea, enlaza y pone en escucha un socket TCP pasivo usando getaddrinfo (IPv4/IPv6)
int server_create_listen_socket(const char *service, int backlog) {
    struct addrinfo hints, *result, *entry;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int error = getaddrinfo(NULL, service, &hints, &result);
    if (error != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return -1;
    }

    int server = -1;
    for (entry = result; entry; entry = entry->ai_next) {
        server = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (server < 0) continue;
        int yes = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(server, entry->ai_addr, entry->ai_addrlen) == 0 &&
            listen(server, backlog) == 0) {
            break;
        }
        close(server);
        server = -1;
    }
    freeaddrinfo(result);

    if (server < 0) {
        fprintf(stderr, "No se pudo enlazar el puerto %s: %s\n", service, strerror(errno));
    }
    return server;
}

// Configura un descriptor en modo no bloqueante para epoll
int server_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int server_set_epoll_events(int epfd, int fd, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

void server_init_clients(client_t *clients, size_t count) {
    for (size_t i = 0; i < count; i++) {
        clients[i].fd = -1;
        clients[i].used = 0;
        clients[i].last_activity = 0;
        clients[i].sending = 0;
        clients[i].close_after_send = 0;
        clients[i].out_len = 0;
        clients[i].out_sent = 0;
        clients[i].file_fd = -1;
    }
}

client_t *server_find_client(client_t *clients, size_t count, int fd) {
    for (size_t i = 0; i < count; i++) {
        if (clients[i].fd == fd) return &clients[i];
    }
    return NULL;
}

// Cierra el socket del cliente, su archivo abierto (si hay) y limpia el slot
void server_close_client(int epfd, client_t *client) {
    if (client->file_fd >= 0) {
        close(client->file_fd);
        client->file_fd = -1;
    }
    if (client->fd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
        client->fd = -1;
    }
    client->used = 0;
    client->sending = 0;
    client->close_after_send = 0;
    client->out_len = 0;
    client->out_sent = 0;
}

static int header_contains_ci(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            char before = (p == haystack) ? '\0' : p[-1];
            char after = p[needle_len];
            int valid_before = (p == haystack || before == ',' || isspace((unsigned char)before));
            int valid_after = (after == '\0' || after == ',' || isspace((unsigned char)after));
            if (valid_before && valid_after) return 1;
        }
    }
    return 0;
}

void server_queue_error_response(client_t *client, int status, const char *detail,
                                 int keep_alive, const char *extra) {
    int n = http_format_error(client->out_buf, sizeof(client->out_buf),
                              status, detail, keep_alive, extra);
    if (n < 0) {
        client->out_len = 0;
        client->out_sent = 0;
        client->sending = 0;
        client->close_after_send = 1;
        return;
    }
    client->out_len = (size_t)n;
    client->out_sent = 0;
    client->file_fd = -1;
    client->sending = 1;
    client->close_after_send = !keep_alive;
}

// Prepara la respuesta en out_buf: valida GET, cabeceras HTTP/1.1 obligatorias y abre el archivo
static void prepare_response(client_t *client, const char *root, char *raw) {
    http_request_t request;
    int parse = http_parse_request(raw, strlen(raw), &request);
    if (parse == 505) {
        server_queue_error_response(client, 505, "Versión HTTP no soportada.", 0, NULL);
        return;
    }
    if (parse != 0) {
        server_queue_error_response(client, 400, "Solicitud HTTP malformada.", 0, NULL);
        return;
    }

    char conn_val[128];
    int has_conn = http_get_header(raw, "Connection", conn_val, sizeof(conn_val));
    if (has_conn && header_contains_ci(conn_val, "close")) {
        request.keep_alive = 0;
    }

    if (strcmp(request.method, "GET") != 0) {
        server_queue_error_response(client, 405, "Este servidor solo procesa GET.",
                                    request.keep_alive, "Allow: GET\r\n");
        return;
    }

    if (!http_has_header(raw, "Host") ||
        !has_conn ||
        !http_has_header(raw, "User-Agent")) {
        server_queue_error_response(client, 400, "HTTP/1.1 requiere Host, Connection y User-Agent.", 0, NULL);
        return;
    }

    int file_fd;
    struct stat info;
    char path[4096];
    file_status_t status = file_open_safe(root, request.target, &file_fd, &info, path, sizeof(path));
    if (status != FILE_OK) {
        const char *msg = status == FILE_FORBIDDEN ? "Acceso denegado." :
                          status == FILE_NOT_FOUND ? "El recurso no existe." :
                                                     "Error interno.";
        server_queue_error_response(client, status, msg, request.keep_alive, NULL);
        return;
    }

    int hdr_len = http_format_headers(client->out_buf, sizeof(client->out_buf),
                                      200, mime_type_for_path(path),
                                      info.st_size, request.keep_alive, NULL);
    if (hdr_len < 0) {
        close(file_fd);
        server_queue_error_response(client, 500, "Error interno.", 0, NULL);
        return;
    }

    client->out_len = (size_t)hdr_len;
    client->out_sent = 0;
    client->file_fd = file_fd;
    client->sending = 1;
    client->close_after_send = !request.keep_alive;
}

// Envía cabeceras y bloques del archivo de forma no bloqueante
// Retorna: 1 = terminó envío, 0 = pendiente (EAGAIN), -1 = error de socket
int server_flush_client_write(client_t *client, time_t now) {
    while (client->sending) {
        if (client->out_sent < client->out_len) {
            ssize_t n = send(client->fd,
                             client->out_buf + client->out_sent,
                             client->out_len - client->out_sent,
                             MSG_NOSIGNAL);
            if (n > 0) {
                client->out_sent += (size_t)n;
                client->last_activity = now;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
            return -1;
        }

        if (client->file_fd >= 0) {
            ssize_t r = read(client->file_fd, client->out_buf, sizeof(client->out_buf));
            if (r > 0) {
                client->out_len = (size_t)r;
                client->out_sent = 0;
                continue;
            }
            if (r < 0 && errno == EINTR) continue;
            close(client->file_fd);
            client->file_fd = -1;
            if (r < 0) return -1;
        }

        client->sending = 0;
        client->out_len = 0;
        client->out_sent = 0;
        return 1;
    }
    return 1;
}

// Extrae y atiende peticiones completas (\r\n\r\n) acumuladas en el buffer del cliente
void server_process_pending_requests(int epfd, client_t *client, const char *root, time_t now) {
    char *end;
    while (client->fd >= 0 && !client->sending &&
           (end = strstr(client->buffer, "\r\n\r\n")) != NULL) {
        size_t request_length = (size_t)(end - client->buffer) + 4;
        char request[MINIHTTPD_REQUEST_MAX + 1];
        memcpy(request, client->buffer, request_length);
        request[request_length] = '\0';

        size_t remaining = client->used - request_length;
        memmove(client->buffer, client->buffer + request_length, remaining);
        client->used = remaining;
        client->buffer[remaining] = '\0';

        prepare_response(client, root, request);
        if (!client->sending) {
            server_close_client(epfd, client);
            return;
        }

        int wr = server_flush_client_write(client, now);
        if (wr < 0) {
            server_close_client(epfd, client);
            return;
        }
        if (wr == 0) {
            if (server_set_epoll_events(epfd, client->fd, EPOLLOUT) < 0) {
                server_close_client(epfd, client);
            }
            return;
        }
        if (client->close_after_send) {
            server_close_client(epfd, client);
            return;
        }
    }
}
