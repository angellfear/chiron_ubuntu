# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Cross-built by scripts/builder/build-bt.sh with the Fenix toolchain, the same
# one the kernel and the out-of-tree modules use. GNU make defines CC=cc
# internally, so ?= would silently produce a host binary; callers override it.
CC = aarch64-none-linux-gnu-gcc
CFLAGS ?= -O2 -pipe -Wall -Wextra -Werror
CPPFLAGS ?=
LDFLAGS ?= -s

.PHONY: all clean

all: chiron-bt-init

chiron-bt-init: chiron-bt-init.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f chiron-bt-init
