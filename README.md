# MiniHTTPd

## 1. Descripción del proyecto

**MiniHTTPd** es un servidor HTTP/1.1 de contenido estático para Linux desarrollado en C, sin bibliotecas HTTP externas. Implementa concurrencia de un solo hilo basada en eventos (`epoll`), sockets TCP no bloqueantes, conexiones persistentes, resolución segura de archivos, tipos MIME y mitigación de vulnerabilidades comunes.

---

## 2. Requisitos implementados (Checklist de cumplimiento)

| Requisito solicitado | Implementación | Ubicación en el código | Estado |
| :--- | :--- | :--- | :---: |
| Servidor HTTP sin bibliotecas externas | Sockets POSIX, parser y formateador HTTP propios | `src/passivesock.c`, `src/http.c` | ✅ |
| Concurrencia con `epoll` (1 hilo) | Bucle de eventos con `epoll_create1`, `epoll_ctl`, `epoll_wait` y `O_NONBLOCK` | `src/main.c` (`server_run`, `set_nonblocking`) | ✅ |
| Conexiones persistentes | `keep-alive` por defecto en HTTP/1.1, cola de peticiones y cierre con `Connection: close` | `src/http.c` (`http_parse_request`), `src/main.c` (`process_pending_requests`) | ✅ |
| Exclusividad de protocolo `HTTP/1.1` | Rechaza `HTTP/1.0`, `HTTP/2.0` u otras versiones con `505` | `src/http.c` (`http_parse_request`) | ✅ |
| Solo método `GET` | Rechaza cualquier otro método (`POST`, `PUT`, `HEAD`, etc.) con `405` y `Allow: GET` | `src/main.c` (`prepare_response`) | ✅ |
| Cabeceras obligatorias HTTP/1.1 | Exige `Host`, `Connection` y `User-Agent` presentes y no vacías (`400` si faltan) | `src/main.c` (`prepare_response`), `src/http.c` (`http_get_header`) | ✅ |
| Tipos MIME | `.html`, `.css`, `.js`, `.png`, `.jpg` y extensiones adicionales | `src/mime.c` (`mime_type_for_path`) | ✅ |
| Códigos de estado requeridos | `200`, `400`, `403`, `404`, `405`, `500` (además `503` y `505`) | `src/http.c` (`http_reason_phrase`, `http_format_error`), `src/files.c` (`map_errno`) | ✅ |
| Control de tamaño de solicitudes | Límite de buffer (`8192` B) y URI (`4096` B) respondiendo `400 Bad Request` | `include/config.h`, `include/http.h`, `src/http.c`, `src/main.c` | ✅ |
| Estructura del repositorio | `Makefile`, `README`, `include/`, `src/`, `www/` | Raíz de `MiniHTTPd/` | ✅ |

---

## 3. Arquitectura del proyecto

```text
MiniHTTPd/
├── Makefile                  # Compilación 
├── README.md                 # Documentaciòn
├── include/
│   ├── config.h              # Puerto por defecto, raíz web, límites y timeouts
│   ├── files.h               # Estados de archivo y prototipo de apertura segura
│   ├── http.h                # Estructura http_request_t, parser y formateo HTTP
│   └── mime.h                # Prototipo de resolución de tipos MIME
├── src/
│   ├── main.c                # Arranque, bucle epoll, E/S no bloqueante, keep-alive y timeouts
│   ├── http.c                # Parsing de solicitud HTTP/1.1 y generación de respuestas
│   ├── files.c               # Decodificación URI, realpath() y apertura de archivos
│   ├── mime.c                # Mapeo de extensiones a Content-Type
│   ├── passivesock.c         # Creación y bind de socket TCP con getaddrinfo()
│   └── passiveTCP.c          # Adaptador TCP hacia passivesock()
└── www/
    ├── index.html            
    ├── style.css            
    ├── app.js               
    └── image.png            
```

## 4. Responsabilidad de los módulos principales

- **`src/main.c`**: valida argumentos de línea de comandos, resuelve la ruta canónica de `www/` con `realpath()`, verifica que sea directorio, abre el socket TCP con `passiveTCP()` y ejecuta el bucle `epoll` para administrar hasta `MINIHTTPD_MAX_CLIENTS` (`128`) conexiones simultáneas en un solo hilo, manejando escrituras parciales con `EPOLLOUT`, timeout de inactividad (`15 s`), peticiones persistentes y reglas HTTP/1.1 (`GET` y 3 cabeceras obligatorias).
- **`src/http.c`**: valida sintaxis de línea de solicitud y cabeceras (`http_parse_request`), consulta cabeceras sin distinguir mayúsculas (`http_get_header`) y construye cabeceras y páginas HTML de error (`http_format_headers`, `http_format_error`).
- **`src/files.c`**: limpia query strings, decodifica `%XX` rechazando caracteres de control (`<= 0x1F`, `0x7F`), bloquea segmentos `..`, resuelve la ruta final con `realpath()` y verifica con `inside_root()` que no escape de `www/`.
- **`src/mime.c`**: determina el `Content-Type` según la extensión del archivo (sin distinguir mayúsculas) con respaldo `application/octet-stream`.

---

## 5. Ejecución

```bash
make
./minihttpd 8080 www
```
Sin argumentos utiliza por defecto el puerto `8080` y el directorio `www` (`./minihttpd`).  
Reglas disponibles: `make` (compilar), `make clean` (limpiar binarios y objetos) y `make re` (recompilar).

---