CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror

.PHONY: all check clean

all: gemini-jpeg-qual

gemini-jpeg-qual: gemini-jpeg-qual.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

check: gemini-jpeg-qual
	./gemini-jpeg-qual --selftest

clean:
	$(RM) gemini-jpeg-qual
