#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "config.h"
#include "files.h"
#include "http.h"
#include "mime.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// Estado de cada conexión activa manejada por epoll
typedef struct {
    int fd;
    char buffer[MINIHTTPD_REQUEST_MAX];
    size_t used;
    time_t last_activity;

    int sending;
    int close_after_send;
    char out_buf[MINIHTTPD_FILE_BUFFER];
    size_t out_len;
    size_t out_sent;
    int file_fd;
} client_t;

// Configura un descriptor en modo no bloqueante para epoll
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_epoll_events(int epfd, int fd, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

// Cierra el socket del cliente, su archivo abierto (si hay) y limpia el slot
static void close_client(int epfd, client_t *client) {
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

static client_t *find_client(client_t *clients, int fd) {
    for (int i = 0; i < MINIHTTPD_MAX_CLIENTS; i++) {
        if (clients[i].fd == fd) return &clients[i];
    }
    return NULL;
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

static void queue_error_response(client_t *client, int status, const char *detail,
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
        queue_error_response(client, 505, "Versión HTTP no soportada.", 0, NULL);
        return;
    }
    if (parse != 0) {
        queue_error_response(client, 400, "Solicitud HTTP malformada.", 0, NULL);
        return;
    }

    char conn_val[128];
    int has_conn = http_get_header(raw, "Connection", conn_val, sizeof(conn_val));
    if (has_conn && header_contains_ci(conn_val, "close")) {
        request.keep_alive = 0;
    }

    if (strcmp(request.method, "GET") != 0) {
        queue_error_response(client, 405, "Este servidor solo procesa GET.",
                             request.keep_alive, "Allow: GET\r\n");
        return;
    }

    if (strcmp(request.version, "HTTP/1.1") == 0) {
        if (!http_has_header(raw, "Host") ||
            !has_conn ||
            !http_has_header(raw, "User-Agent")) {
            queue_error_response(client, 400, "HTTP/1.1 requiere Host, Connection y User-Agent.", 0, NULL);
            return;
        }
    }

    int file_fd;
    struct stat info;
    char path[4096];
    file_status_t status = file_open_safe(root, request.target, &file_fd, &info, path, sizeof(path));
    if (status != FILE_OK) {
        const char *msg = status == FILE_FORBIDDEN ? "Acceso denegado." :
                          status == FILE_NOT_FOUND ? "El recurso no existe." :
                                                     "Error interno.";
        queue_error_response(client, status, msg, request.keep_alive, NULL);
        return;
    }

    int hdr_len = http_format_headers(client->out_buf, sizeof(client->out_buf),
                                      200, mime_type_for_path(path),
                                      info.st_size, request.keep_alive, NULL);
    if (hdr_len < 0) {
        close(file_fd);
        queue_error_response(client, 500, "Error interno.", 0, NULL);
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
static int flush_client_write(client_t *client, time_t now) {
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
static void process_pending_requests(int epfd, client_t *client, const char *root, time_t now) {
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
            close_client(epfd, client);
            return;
        }

        int wr = flush_client_write(client, now);
        if (wr < 0) {
            close_client(epfd, client);
            return;
        }
        if (wr == 0) {
            if (set_epoll_events(epfd, client->fd, EPOLLOUT) < 0) {
                close_client(epfd, client);
            }
            return;
        }
        if (client->close_after_send) {
            close_client(epfd, client);
            return;
        }
    }
}

// Bucle principal event-driven con epoll: acepta conexiones, controla timeouts y despacha E/S
void server_run(int listen_fd, const char *root) {
    signal(SIGPIPE, SIG_IGN);

    if (set_nonblocking(listen_fd) < 0) {
        perror("fcntl listen_fd");
        return;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return;
    }

    struct epoll_event listen_ev;
    memset(&listen_ev, 0, sizeof(listen_ev));
    listen_ev.events = EPOLLIN;
    listen_ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &listen_ev) < 0) {
        perror("epoll_ctl listen_fd");
        close(epfd);
        return;
    }

    client_t clients[MINIHTTPD_MAX_CLIENTS];
    for (int i = 0; i < MINIHTTPD_MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].used = 0;
        clients[i].last_activity = 0;
        clients[i].sending = 0;
        clients[i].close_after_send = 0;
        clients[i].out_len = 0;
        clients[i].out_sent = 0;
        clients[i].file_fd = -1;
    }

    struct epoll_event events[MINIHTTPD_MAX_CLIENTS + 1];

    for (;;) {
        int nfds = epoll_wait(epfd, events, MINIHTTPD_MAX_CLIENTS + 1, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        time_t now = time(NULL);
        for (int i = 0; i < MINIHTTPD_MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 && now - clients[i].last_activity > MINIHTTPD_CLIENT_TIMEOUT) {
                close_client(epfd, &clients[i]);
            }
        }

        for (int i = 0; i < nfds; i++) {
            int ev_fd = events[i].data.fd;
            uint32_t ev_flags = events[i].events;

            if (ev_fd == listen_fd) {
                for (;;) {
                    int fd = accept(listen_fd, NULL, NULL);
                    if (fd < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (set_nonblocking(fd) < 0) {
                        close(fd);
                        continue;
                    }

                    int placed = 0;
                    for (int c = 0; c < MINIHTTPD_MAX_CLIENTS; c++) {
                        if (clients[c].fd < 0) {
                            struct epoll_event cev;
                            memset(&cev, 0, sizeof(cev));
                            cev.events = EPOLLIN;
                            cev.data.fd = fd;
                            if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &cev) == 0) {
                                clients[c].fd = fd;
                                clients[c].used = 0;
                                clients[c].last_activity = now;
                                clients[c].sending = 0;
                                clients[c].close_after_send = 0;
                                clients[c].out_len = 0;
                                clients[c].out_sent = 0;
                                clients[c].file_fd = -1;
                                placed = 1;
                            }
                            break;
                        }
                    }
                    if (!placed) {
                        http_send_error(fd, 503, "Demasiados clientes.", 0, NULL);
                        close(fd);
                    }
                }
                continue;
            }

            client_t *client = find_client(clients, ev_fd);
            if (!client) continue;

            if (!(ev_flags & (EPOLLIN | EPOLLOUT)) && (ev_flags & (EPOLLERR | EPOLLHUP))) {
                close_client(epfd, client);
                continue;
            }

            if ((ev_flags & EPOLLOUT) && client->sending) {
                int wr = flush_client_write(client, now);
                if (wr < 0) {
                    close_client(epfd, client);
                    continue;
                }
                if (wr == 1) {
                    if (client->close_after_send) {
                        close_client(epfd, client);
                        continue;
                    }
                    if (set_epoll_events(epfd, client->fd, EPOLLIN) < 0) {
                        close_client(epfd, client);
                        continue;
                    }
                    process_pending_requests(epfd, client, root, now);
                }
                continue;
            }

            if (!(ev_flags & EPOLLIN) || client->sending) continue;

            for (;;) {
                if (client->used >= sizeof(client->buffer) - 1) break;
                ssize_t received = recv(client->fd,
                                        client->buffer + client->used,
                                        sizeof(client->buffer) - client->used - 1,
                                        0);
                if (received > 0) {
                    client->used += (size_t)received;
                    client->buffer[client->used] = '\0';
                    client->last_activity = now;
                    continue;
                }
                if (received < 0 && errno == EINTR) continue;
                if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                close_client(epfd, client);
                break;
            }

            if (client->fd < 0) continue;

            if (client->used == sizeof(client->buffer) - 1 && !strstr(client->buffer, "\r\n\r\n")) {
                queue_error_response(client, 400, "Solicitud demasiado grande.", 0, NULL);
                int wr = flush_client_write(client, now);
                if (wr == 0) {
                    if (set_epoll_events(epfd, client->fd, EPOLLOUT) < 0) {
                        close_client(epfd, client);
                    }
                } else {
                    close_client(epfd, client);
                }
                continue;
            }

            process_pending_requests(epfd, client, root, now);
        }
    }

    for (int i = 0; i < MINIHTTPD_MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) close_client(epfd, &clients[i]);
    }
    close(epfd);
}
