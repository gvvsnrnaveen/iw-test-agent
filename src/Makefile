# Native build:        make
# Cross build example: make CC=aarch64-openwrt-linux-musl-gcc
#   (export STAGING_DIR=<sdk>/staging_dir and put the toolchain bin/ in PATH)
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -Wno-format-truncation -std=gnu99
LDFLAGS ?=

all: iw-test-agent

iw-test-agent: iw-test-agent.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

static: iw-test-agent.c
	$(CC) $(CFLAGS) -static -o iw-test-agent $< $(LDFLAGS)

clean:
	rm -f iw-test-agent

.PHONY: all static clean
