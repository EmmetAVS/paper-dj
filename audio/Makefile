# Builds on QNX (natively on the Pi, or with qcc) and on Linux/macOS for testing.
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11   # gnu11: QNX headers hide some POSIX calls under strict c11
OS     := $(shell uname -s)

ifeq ($(OS),QNX)
  LIBS = -lm -lsocket          # QNX keeps sockets in libsocket; pthreads are in libc
else ifeq ($(OS),Linux)
  CFLAGS += -D_GNU_SOURCE
  LIBS = -lm -lpthread
else
  LIBS = -lm -lpthread
endif

SRC = src/main.c src/mixer.c src/dsp.c src/wav.c src/net.c src/bpm.c src/resmgr.c src/library.c
HDR = $(wildcard src/*.h)

dj_engine: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)

clean:
	rm -f dj_engine

.PHONY: clean
