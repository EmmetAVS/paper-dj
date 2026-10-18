# Builds the vision helper library and the pipeline skeleton.
# Works natively on the QNX Pi and on Linux for testing.
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11 -Iinclude
OS     := $(shell uname -s)

ifeq ($(OS),QNX)
  LIBS = -lm -lsocket
else ifeq ($(OS),Linux)
  CFLAGS += -D_GNU_SOURCE
  LIBS = -lm -lpthread
else
  LIBS = -lm -lpthread
endif

LIB_SRC = src/pdj_rt.c src/pdj_link.c
LIB_OBJ = $(LIB_SRC:.c=.o)

all: libpdj.a vision_skeleton

libpdj.a: $(LIB_OBJ)
	ar rcs $@ $^

src/%.o: src/%.c include/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

vision_skeleton: src/vision_skeleton.c libpdj.a
	$(CC) $(CFLAGS) -o $@ $< libpdj.a $(LIBS)

clean:
	rm -f $(LIB_OBJ) libpdj.a vision_skeleton

.PHONY: all clean
