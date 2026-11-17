#ifndef MINIHTTPD_SERVER_H
#define MINIHTTPD_SERVER_H

// Inicia el bucle principal con epoll para atender clientes concurrentes
void server_run(int listen_fd, const char *document_root);

#endif
