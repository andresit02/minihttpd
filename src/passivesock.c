#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "config.h"

int passivesock(const char *service, const char *transport, int backlog) {
    (void)transport;
    struct addrinfo hints, *result, *entry;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    int error = getaddrinfo(NULL, service, &hints, &result);
    if (error != 0) { fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error)); return -1; }
    int server = -1;
    for (entry = result; entry; entry = entry->ai_next) {
        server = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (server < 0) continue;
        int yes = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(server, entry->ai_addr, entry->ai_addrlen) == 0 && listen(server, backlog) == 0) break;
        close(server); server = -1;
    }
    freeaddrinfo(result);
    if (server < 0) fprintf(stderr, "No se pudo enlazar el puerto %s: %s\n", service, strerror(errno));
    return server;
}
