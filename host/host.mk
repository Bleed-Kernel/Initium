.POSIX:

SHELL=/bin/sh

CC=cc
CFLAGS=-g -O2 -pipe
CPPFLAGS=
LDFLAGS=
LIBS=

.PHONY: all
all: initium

.PHONY: clean
clean:
	rm -f initium initium.exe

initium: initium.c
	$(CC) $(CFLAGS) -std=c99 -D_FILE_OFFSET_BITS=64 $(CPPFLAGS) $(LDFLAGS) initium.c $(LIBS) -o $@
