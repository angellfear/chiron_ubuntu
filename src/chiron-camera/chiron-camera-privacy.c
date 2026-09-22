// SPDX-License-Identifier: 0BSD
// Copyright (c) 2026 angellfear
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#define SWITCH_DEVICE "/dev/input/by-path/platform-gpio-keys-event"
#define CAMERA_DEVICE "/dev/chiron-camera"

static void set_privacy(int closed)
{
    mode_t mode = closed ? 0000 : 0660;
    int attempt;
    int chmod_errno = 0;

    for (attempt = 0; attempt < 100; attempt++) {
        if (chmod(CAMERA_DEVICE, mode) == 0) {
            chmod_errno = 0;
            break;
        }
        chmod_errno = errno;
        if (errno != ENOENT)
            break;
        usleep(100000);
    }
    if (chmod_errno)
        fprintf(stderr, "chiron-camera: cannot chmod %s: %s\n",
                CAMERA_DEVICE, strerror(chmod_errno));
    fprintf(stderr, "chiron-camera: privacy shutter %s\n",
            closed ? "closed" : "open");
}

int main(void)
{
    unsigned long switches[(SW_MAX / (8 * sizeof(unsigned long))) + 1] = {0};
    struct input_event event;
    int fd;
    int state;

    fd = open(SWITCH_DEVICE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "chiron-camera: cannot open %s: %s\n",
                SWITCH_DEVICE, strerror(errno));
        return 1;
    }
    if (ioctl(fd, EVIOCGSW(sizeof(switches)), switches) < 0) {
        fprintf(stderr, "chiron-camera: cannot read switch state: %s\n",
                strerror(errno));
        close(fd);
        return 1;
    }
    state = !!(switches[SW_CAMERA_LENS_COVER / (8 * sizeof(unsigned long))] &
               (1UL << (SW_CAMERA_LENS_COVER %
                        (8 * sizeof(unsigned long)))));
    set_privacy(state);

    for (;;) {
        ssize_t size = read(fd, &event, sizeof(event));

        if (size < 0 && errno == EINTR)
            continue;
        if (size != sizeof(event)) {
            fprintf(stderr, "chiron-camera: switch input ended: %s\n",
                    size < 0 ? strerror(errno) : "short read");
            close(fd);
            return 1;
        }
        if (event.type == EV_SW && event.code == SW_CAMERA_LENS_COVER &&
            state != !!event.value) {
            state = !!event.value;
            set_privacy(state);
        }
    }
}
