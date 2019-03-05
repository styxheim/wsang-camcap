# vim: ft=make ff=unix fenc=utf-8
# file: Makefile
LIBS+=-lev
CFLAGS+=-g -Wall -Werror -pedantic

all: capture dump

clean:
	rm -f capture dump

capture: src/main.c src/circle_buffer.c
	${CC} -o $@ ${CFLAGS} $^ ${LIBS}

dump: src/dump.c
	${CC} -o $@ ${CFLAGS} $^ ${LIBS}
