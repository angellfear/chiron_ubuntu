# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
CC ?= gcc
PKG_CONFIG ?= pkg-config

PACKAGES = gtk+-3.0 gio-2.0 gtk-layer-shell-0
CFLAGS ?= -O2 -pipe
CFLAGS += -Wall -Wextra -Werror $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm

chiron-privacy: chiron-privacy.c chiron-display.c chiron-display.h
	$(CC) $(CFLAGS) -o $@ chiron-privacy.c chiron-display.c $(LDLIBS)

clean:
	rm -f chiron-privacy

.PHONY: clean
