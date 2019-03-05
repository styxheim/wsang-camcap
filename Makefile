# vim: ft=make ff=unix fenc=utf-8
# file: Makefile
LIBS+=-lev
CFLAGS+=-g -Wall -Werror -pedantic

all: capture

capture: src/main.c src/circle_buffer.c
	${CC} -o $@ ${CFLAGS} $^ ${LIBS}

