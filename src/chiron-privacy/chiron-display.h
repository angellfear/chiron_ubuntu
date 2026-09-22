// SPDX-License-Identifier: 0BSD
// Copyright (c) 2026 angellfear
#ifndef CHIRON_DISPLAY_H
#define CHIRON_DISPLAY_H

#include <gio/gio.h>

struct chiron_display;

/* Returns -1 when argv is not a display-control invocation. Otherwise sends
 * the request to the running session controller and returns a process status. */
int chiron_display_remote_command(int argc, char **argv);

struct chiron_display *chiron_display_new(GDBusConnection *bus, int lux,
                                           gboolean lux_known);
void chiron_display_set_lux(struct chiron_display *display, int lux,
                            gboolean known);
void chiron_display_set_privacy(struct chiron_display *display,
                                gboolean camera_closed, gboolean mic_muted);

#endif
