#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "server.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Bucle principal event-driven con epoll: acepta conexiones, controla timeouts y despacha E/S
static void run_event_loop(int listen_fd, const char *root) {
    signal(SIGPIPE, SIG_IGN);

    if (server_set_nonblocking(listen_fd) < 0) {
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
    server_init_clients(clients, MINIHTTPD_MAX_CLIENTS);

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
                server_close_client(epfd, &clients[i]);
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
                    if (server_set_nonblocking(fd) < 0) {
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

            client_t *client = server_find_client(clients, MINIHTTPD_MAX_CLIENTS, ev_fd);
            if (!client) continue;

            if (!(ev_flags & (EPOLLIN | EPOLLOUT)) && (ev_flags & (EPOLLERR | EPOLLHUP))) {
                server_close_client(epfd, client);
                continue;
            }

            if ((ev_flags & EPOLLOUT) && client->sending) {
                int wr = server_flush_client_write(client, now);
                if (wr < 0) {
                    server_close_client(epfd, client);
                    continue;
                }
                if (wr == 1) {
                    if (client->close_after_send) {
                        server_close_client(epfd, client);
                        continue;
                    }
                    if (server_set_epoll_events(epfd, client->fd, EPOLLIN) < 0) {
                        server_close_client(epfd, client);
                        continue;
                    }
                    server_process_pending_requests(epfd, client, root, now);
                }
                continue;
            }

            if (!(ev_flags & EPOLLIN) || client->sending) continue;

            int peer_closed = 0;
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
                if (received == 0) {
                    peer_closed = 1;
                    break;
                }
                server_close_client(epfd, client);
                break;
            }

            if (client->fd < 0) continue;

            if (client->used == sizeof(client->buffer) - 1 && !strstr(client->buffer, "\r\n\r\n")) {
                server_queue_error_response(client, 400, "Solicitud demasiado grande.", 0, NULL);
                int wr = server_flush_client_write(client, now);
                if (wr == 0) {
                    if (server_set_epoll_events(epfd, client->fd, EPOLLOUT) < 0) {
                        server_close_client(epfd, client);
                    }
                } else {
                    server_close_client(epfd, client);
                }
                continue;
            }

            server_process_pending_requests(epfd, client, root, now);
            if (peer_closed && client->fd >= 0 && !client->sending) {
                server_close_client(epfd, client);
            }
        }
    }

    for (int i = 0; i < MINIHTTPD_MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) server_close_client(epfd, &clients[i]);
    }
    close(epfd);
}

int main(int argc, char **argv) {
    const char *port = argc > 1 ? argv[1] : MINIHTTPD_DEFAULT_PORT;
    const char *root = argc > 2 ? argv[2] : MINIHTTPD_DEFAULT_ROOT;
    if (argc > 3) {
        fprintf(stderr, "Uso: %s [puerto] [directorio_web]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Resuelve la ruta absoluta de la raíz web y verifica que sea un directorio
    char resolved_root[PATH_MAX];
    if (!realpath(root, resolved_root)) {
        perror("directorio_web");
        return EXIT_FAILURE;
    }
    struct stat info;
    if (stat(resolved_root, &info) < 0 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "La raíz web no es un directorio.\n");
        return EXIT_FAILURE;
    }

    // Abre el socket en escucha y arranca el bucle con epoll
    int server = server_create_listen_socket(port, MINIHTTPD_BACKLOG);
    if (server < 0) return EXIT_FAILURE;
    printf("Servidor MiniHTTPd escuchando en puerto %s\n", port);
    fflush(stdout);
    run_event_loop(server, resolved_root);
    close(server);
    return EXIT_SUCCESS;
}
