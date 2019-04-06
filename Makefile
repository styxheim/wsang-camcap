# vim: ft=make ff=unix fenc=utf-8
# file: Makefile
LIBS+=-lev -lpthread
CFLAGS+=-g -Wall -Werror -pedantic

all: capture dump extract

clean:
	rm -f capture dump extract

capture: src/main.c \
				 src/circle_buffer.c \
				 src/main_write_thread.c
	${CC} -o $@ ${CFLAGS} $^ ${LIBS}

dump: src/dump.c
	${CC} -o $@ ${CFLAGS} $^ ${LIBS}

extract: src/extract.c
	${CC} -o $@ ${CFLAGS} $^ ${LIBS}
