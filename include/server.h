#ifndef MINIHTTPD_SERVER_H
#define MINIHTTPD_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Parámetros generales de configuración del servidor
#define MINIHTTPD_DEFAULT_PORT "8080"
#define MINIHTTPD_DEFAULT_ROOT "www"
#define MINIHTTPD_BACKLOG 32          // Cola máxima de conexiones pendientes
#define MINIHTTPD_MAX_CLIENTS 128     // Límite de clientes simultáneos en epoll
#define MINIHTTPD_REQUEST_MAX 8192    // Tamaño máximo de una petición HTTP (bytes)
#define MINIHTTPD_FILE_BUFFER 16384   // Buffer de lectura/envío de archivos
#define MINIHTTPD_CLIENT_TIMEOUT 15   // Segundos de inactividad antes de cerrar

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

// Socket en escucha y utilidades de E/S no bloqueante para epoll
int server_create_listen_socket(const char *service, int backlog);
int server_set_nonblocking(int fd);
int server_set_epoll_events(int epfd, int fd, uint32_t events);

// Gestión de clientes activos, despacho de peticiones y envío no bloqueante
void server_init_clients(client_t *clients, size_t count);
client_t *server_find_client(client_t *clients, size_t count, int fd);
void server_close_client(int epfd, client_t *client);
int server_flush_client_write(client_t *client, time_t now);
void server_queue_error_response(client_t *client, int status, const char *detail,
                                 int keep_alive, const char *extra);
void server_process_pending_requests(int epfd, client_t *client, const char *root, time_t now);

#endif
