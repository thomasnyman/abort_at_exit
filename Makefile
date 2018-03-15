#!/usr/bin/make -f

CFLAGS = -g -Wall -Wextra -std=c99 -pedantic -Wall -O2 -fpic -shared
LDFLAGS = -ldl

.PHONY: clean

abort_at_exit.o: abort_at_exit.c
	${CC} ${CFLAGS} $< -o $@ ${LDFLAGS}

clean:
	${RM} abort_at_exit.o

