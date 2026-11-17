#ifndef MINIHTTPD_MIME_H
#define MINIHTTPD_MIME_H

// Devuelve el tipo Content-Type según la extensión del archivo
const char *mime_type_for_path(const char *path);

#endif
