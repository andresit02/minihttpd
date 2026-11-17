CC := gcc
CPPFLAGS := -Iinclude -MMD -MP
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS :=
LDLIBS :=

TARGET := minihttpd
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

.PHONY: all clean re

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(DEP) $(TARGET)

re: clean all

-include $(DEP)
