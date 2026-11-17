#ifndef MINIHTTPD_CONFIG_H
#define MINIHTTPD_CONFIG_H

// Parámetros generales de configuración del servidor
#define MINIHTTPD_DEFAULT_PORT "8080"
#define MINIHTTPD_DEFAULT_ROOT "www"
#define MINIHTTPD_BACKLOG 32          // Cola máxima de conexiones pendientes
#define MINIHTTPD_MAX_CLIENTS 128     // Límite de clientes simultáneos en epoll
#define MINIHTTPD_REQUEST_MAX 8192    // Tamaño máximo de una petición HTTP (bytes)
#define MINIHTTPD_FILE_BUFFER 16384   // Buffer de lectura/envío de archivos
#define MINIHTTPD_CLIENT_TIMEOUT 15   // Segundos de inactividad antes de cerrar
#define MINIHTTPD_SERVER_NAME "MiniHTTPd/2.0"

#endif
