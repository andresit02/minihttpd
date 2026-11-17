#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "server.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int passiveTCP(const char *service, int backlog);

int main(int argc, char **argv) {
    const char *port = argc > 1 ? argv[1] : MINIHTTPD_DEFAULT_PORT;
    const char *root = argc > 2 ? argv[2] : MINIHTTPD_DEFAULT_ROOT;
    if (argc > 3) { fprintf(stderr, "Uso: %s [puerto] [directorio_web]\n", argv[0]); return EXIT_FAILURE; }

    // Resuelve la ruta absoluta de la raíz web y verifica que sea un directorio
    char resolved_root[PATH_MAX];
    if (!realpath(root, resolved_root)) { perror("directorio_web"); return EXIT_FAILURE; }
    struct stat info;
    if (stat(resolved_root, &info) < 0 || !S_ISDIR(info.st_mode)) { fprintf(stderr, "La raíz web no es un directorio.\n"); return EXIT_FAILURE; }

    // Abre el socket en escucha y arranca el bucle con epoll
    int server = passiveTCP(port, MINIHTTPD_BACKLOG);
    if (server < 0) return EXIT_FAILURE;
    printf("MiniHTTPd concurrente con epoll en puerto %s\nRaíz web: %s\n", port, resolved_root);
    fflush(stdout);
    server_run(server, resolved_root);
    close(server);
    return EXIT_SUCCESS;
}
