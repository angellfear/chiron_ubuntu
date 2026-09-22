# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
# Cross-built by scripts/builder/build-panfrost-drain.sh with the Fenix
# toolchain. DRM_UAPI is the kernel's include/uapi directory copy prepared by
# that script: the toolchain sysroot has no panfrost_drm.h. The uapi headers
# are unsanitised, hence the empty __user.
CC = aarch64-none-linux-gnu-gcc
DRM_UAPI ?= uapi
CFLAGS ?= -O2 -pipe -Wall -Wextra -Werror
CPPFLAGS ?= -I$(DRM_UAPI) -D__user=
LDFLAGS ?= -s -shared -Wl,-z,defs
LDLIBS ?= -ldl -lpthread

.PHONY: all clean

all: libpanfrost-drain.so

libpanfrost-drain.so: panfrost-drain.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f libpanfrost-drain.so
