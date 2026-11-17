# MiniHTTPd

Servidor HTTP/1.1 estático para Linux escrito en C (`-std=c11`) desde cero, sin bibliotecas HTTP externas. Implementa concurrencia de un solo hilo con `epoll`, sockets no bloqueantes, conexiones persistentes, validación estricta de solicitudes, tipos MIME y mitigación de vulnerabilidades.

## Estructura

```text
MiniHTTPd/
├── Makefile                  # Compilación incremental (-MMD -MP)
├── README.md
├── include/
│   ├── config.h              # Puertos, límites y timeouts
│   ├── files.h               # Acceso seguro al sistema de archivos
│   ├── http.h                # Parser HTTP y formateo de respuestas
│   ├── mime.h                # Resolución de tipos MIME
│   └── server.h              # Bucle de eventos epoll
├── src/
│   ├── main.c                # Entrada y validación de raíz web
│   ├── server.c              # epoll, O_NONBLOCK, EPOLLOUT y persistencia
│   ├── http.c                # Parser estricto (request-line y cabeceras)
│   ├── files.c               # realpath(), prevención de traversal y lectura
│   ├── mime.c                # Tabla de tipos MIME
│   ├── passiveTCP.c          # Adaptador TCP
│   └── passivesock.c         # Socket pasivo con getaddrinfo()
└── www/
    ├── index.html
    ├── style.css
    ├── app.js
    └── image.png
```

## Compilación y ejecución

```bash
make
./minihttpd 8080 www
```

Sin argumentos utiliza por defecto el puerto `8080` y el directorio `www` (`./minihttpd`).  
Reglas disponibles: `make` (compilar), `make clean` (limpiar binarios y objetos) y `make re` (recompilar).

## Características implementadas

- **Concurrencia (`epoll`, 1 proceso / 1 hilo):** sin `fork` ni `pthread`. Usa `epoll_create1`, `epoll_ctl` y `epoll_wait` con sockets `O_NONBLOCK`, escritura asíncrona mediante `EPOLLOUT` para archivos grandes o clientes lentos, y cierre por inactividad a los `15 s`.
- **Protocolo y persistencia:**
  - Solo acepta `GET`; cualquier otro método devuelve `405 Method Not Allowed` (`Allow: GET`), evaluado antes de exigir cabeceras opcionales.
  - En `HTTP/1.1` exige `Host`, `Connection` y `User-Agent` presentes y no vacíos (`400 Bad Request` si falta alguno) y mantiene la conexión abierta salvo `Connection: close`.
  - En `HTTP/1.0` no exige esas cabeceras y cierra la conexión al responder. Las respuestas usan línea de estado `HTTP/1.1` (RFC 9112/2145).
- **Sistema de archivos y MIME:** sirve recursos estáticos desde `www/` (`index.html` por defecto en `/` y subdirectorios, sin listado de carpetas). Soporta `.html`/`.htm` (`text/html`), `.css` (`text/css`), `.js` (`application/javascript`), `.png` (`image/png`), `.jpg`/`.jpeg` (`image/jpeg`), además de `.json`, `.txt`, `.xml`, `.gif`, `.svg`, `.ico`, `.pdf` y `application/octet-stream`.
- **Seguridad:**
  - *Directory Traversal:* bloqueo mediante validación de ruta, decodificación `%XX` segura (`%00` y control rechazados) y `realpath()` dentro de `www/` (`403 Forbidden`, incluidos enlaces simbólicos).
  - *Buffer Overflows:* sin `strcpy` ni `sprintf`; parser manual estricto que rechaza sintaxis inválida o tamaños excesivos (`HTTP_TARGET_MAX = 4096`, `MINIHTTPD_REQUEST_MAX = 8192`) con `400 Bad Request`.
- **Códigos de estado:** `200 OK`, `400 Bad Request`, `403 Forbidden`, `404 Not Found`, `405 Method Not Allowed`, `500 Internal Server Error`, `503 Service Unavailable` y `505 HTTP Version Not Supported`.
