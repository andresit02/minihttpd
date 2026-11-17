# Compilador y banderas (C11 estricto + generación automática de dependencias .d)
CC := gcc
CPPFLAGS := -Iinclude -MMD -MP
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS :=
LDLIBS :=

# Binario final y listas de fuentes/objetos/dependencias
TARGET := minihttpd
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

.PHONY: all clean re

all: $(TARGET)

# Enlazado final del ejecutable
$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $@

# Compilación de cada .c a .o
%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Limpieza de artefactos generados y recompilación completa
clean:
	rm -f $(OBJ) $(DEP) $(TARGET)

re: clean all

# Incluye dependencias de cabeceras para recompilar solo lo necesario
-include $(DEP)
