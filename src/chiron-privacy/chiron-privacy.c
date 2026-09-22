// SPDX-License-Identifier: 0BSD
// Copyright (c) 2026 angellfear
/*
 * chiron-privacy -- session tray indicator for the board's two hardware
 * privacy switches, ambient-light policy, display settings and idle clock.
 *
 * It reports and nothing else. The camera shutter and the microphone switch
 * cut their lines electrically, so there is nothing here to toggle: the icon
 * says what the hardware has already done. Taking access to the camera node
 * away when the shutter closes is a separate, system-level job and belongs to
 * chiron-camera-privacy; this is the part a person can see.
 *
 * One tray item carries both switches. It is always the same pair of glyphs
 * side by side -- a webcam and a microphone -- and each is drawn in the panel's
 * ink while its line is live and struck through in red once it is cut, so all
 * four combinations read at a glance without the eye having to work out which
 * of two icons went missing:
 *
 *   both open      webcam, microphone
 *   shutter shut   webcam struck through, microphone
 *   mic muted      webcam, microphone struck through
 *   both           both struck through
 *
 * A tap opens display settings. The item is exported by chiron-display.c,
 * which owns this process's bus connection and answers the properties from the
 * state pushed in here; the icon changes but the item is never added or
 * removed, because the archive wf-panel (wf-shell 0.10) aborts in
 * cairo_surface_reference when a tray item appears or disappears while it is
 * running -- the same buffer-release race the idle clock hits. An icon that
 * only changes never makes the panel rebuild its widget list. The state still
 * reads: a struck-through glyph plus NeedsAttention is not something a glance
 * skips.
 *
 * The earlier arrangement gave each switch its own icon in its own child
 * process, because a StatusNotifierItem is one well-known bus name and one
 * /StatusNotifierItem object and a single GDBusConnection can host only one of
 * them -- and the main connection was already spent on a light-sensor item.
 * Merging the two switches into one item and dropping the light-sensor icon
 * leaves exactly one item, which the main process can host itself.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <glib-unix.h>
#include <gtk/gtk.h>

#include "chiron-display.h"

/* The gpio-keys node carries both switches: SW_CAMERA_LENS_COVER (GPIOY_17)
 * and SW_MUTE_DEVICE (GPIOY_52) are bits 9 and 14 of the same device, which
 * reads back as capabilities/sw = 0x4200. chiron-camera-privacy opens the same
 * path; several readers of one evdev node is how evdev is meant to be used. */
#define SWITCH_DEVICE "/dev/input/by-path/platform-gpio-keys-event"

/* The vendor ALS driver publishes illuminance as ABS_X on an evdev node named
 * "als". The controller and graph use lux directly. */
#define ALS_NAME "als"

#define NOTIFY_NAME "org.freedesktop.Notifications"
#define NOTIFY_PATH "/org/freedesktop/Notifications"

/* Bubble icons. The tray icon merges both switches; a notification is about
 * the one switch that just moved, so it keeps that switch's own glyph. */
#define CAMERA_ICON_OPEN     "camera-web-symbolic"
#define CAMERA_ICON_COVERED  "camera-disabled-symbolic"
#define MIC_ICON_ON          "audio-input-microphone-symbolic"
#define MIC_ICON_OFF         "microphone-disabled-symbolic"

struct state {
    GDBusConnection *bus;

    int camera_closed;
    int mic_muted;

    /* The last bubble each source posted, so each replaces its own. */
    guint32 camera_notify_id;
    guint32 mic_notify_id;

    struct chiron_display *display;
};

/* --- notifications ------------------------------------------------------ */

static void notify_done(GObject *source, GAsyncResult *result, gpointer data)
{
    guint32 *slot = data;
    GVariant *reply;
    GError *error = NULL;

    reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                          &error);
    if (!reply) {
        /* A session without a notification daemon is a working session; the
         * icon is the primary display and the bubble is the reminder. */
        g_message("chiron-privacy: no notification posted: %s", error->message);
        g_error_free(error);
        return;
    }
    g_variant_get(reply, "(u)", slot);
    g_variant_unref(reply);
}

/* Each source replaces its own last bubble rather than stacking a new one:
 * a switch that is flipped twice has one current state, not two events, and a
 * column of contradictory bubbles is how a notification stops being read. */
static void notify(struct state *state, guint32 *slot, const char *icon,
                   const char *summary, const char *body)
{
    GVariantBuilder actions;
    GVariantBuilder hints;

    if (!state->bus)
        return;

    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    /* Device state, not a message from a person: this is what tells swaync to
     * file it as such rather than as chat. */
    g_variant_builder_add(&hints, "{sv}", "category",
                          g_variant_new_string("device"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(1));

    g_dbus_connection_call(state->bus, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_NAME,
                           "Notify",
                           g_variant_new("(susssasa{sv}i)",
                                         "Chiron", *slot, icon, summary, body,
                                         &actions, &hints, 5000),
                           G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, notify_done, slot);
}

/* --- the two privacy switches ------------------------------------------- */

static void privacy_changed(struct state *state)
{
    chiron_display_set_privacy(state->display, state->camera_closed,
                               state->mic_muted);
}

static void camera_changed(struct state *state, gboolean announce)
{
    privacy_changed(state);
    if (announce)
        notify(state, &state->camera_notify_id,
               state->camera_closed ? CAMERA_ICON_COVERED : CAMERA_ICON_OPEN,
               state->camera_closed ? "Camera covered" : "Camera uncovered",
               state->camera_closed
                   ? "The shutter is closed and the camera is disconnected."
                   : "The shutter is open.");
}

static void microphone_changed(struct state *state, gboolean announce)
{
    privacy_changed(state);
    if (announce)
        notify(state, &state->mic_notify_id,
               state->mic_muted ? MIC_ICON_OFF : MIC_ICON_ON,
               state->mic_muted ? "Microphone off" : "Microphone on",
               state->mic_muted
                   ? "The switch is set to off and the microphone is "
                     "disconnected."
                   : "The switch is set to on.");
}

/* Camera and microphone switch movement is physical user activity. swayidle
 * does not consume EV_SW as a normal seat event, so route both positions of
 * both switches through the same wake path used by its resume hook. */
static void wake_from_privacy_switch(void)
{
    GError *error = NULL;

    if (!g_spawn_command_line_async("/usr/local/bin/chiron-idle wake", &error)) {
        g_warning("chiron-privacy: cannot wake display: %s", error->message);
        g_error_free(error);
    }
}

static int switch_bit(const unsigned long *switches, int code)
{
    const size_t bits = 8 * sizeof(unsigned long);

    return !!(switches[code / bits] & (1UL << (code % bits)));
}

/* Read the current position of both switches, not just wait for the next
 * change. This process starts with the session and can be restarted at any
 * point in it, and a switch that has not moved since boot emits nothing: an
 * indicator built only from events shows "open" for a shutter that has been
 * closed all day. */
static gboolean switches_read(int fd, struct state *state)
{
    unsigned long switches[(SW_MAX / (8 * sizeof(unsigned long))) + 1] = {0};

    if (ioctl(fd, EVIOCGSW(sizeof(switches)), switches) < 0) {
        g_warning("chiron-privacy: cannot read switch state: %s",
                  g_strerror(errno));
        return FALSE;
    }
    state->camera_closed = switch_bit(switches, SW_CAMERA_LENS_COVER);
    state->mic_muted = switch_bit(switches, SW_MUTE_DEVICE);
    return TRUE;
}

static gboolean on_switch_event(gint fd, GIOCondition condition, gpointer data)
{
    struct state *state = data;
    struct input_event events[16];
    ssize_t size;
    size_t i;

    if (condition & (G_IO_ERR | G_IO_HUP)) {
        g_warning("chiron-privacy: the switch device went away");
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    size = read(fd, events, sizeof(events));
    if (size < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return G_SOURCE_CONTINUE;
        g_warning("chiron-privacy: switch input ended: %s", g_strerror(errno));
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    for (i = 0; i < (size_t)size / sizeof(events[0]); i++) {
        if (events[i].type != EV_SW)
            continue;
        if (events[i].code == SW_CAMERA_LENS_COVER &&
            state->camera_closed != !!events[i].value) {
            state->camera_closed = !!events[i].value;
            camera_changed(state, TRUE);
            wake_from_privacy_switch();
        } else if (events[i].code == SW_MUTE_DEVICE &&
                   state->mic_muted != !!events[i].value) {
            state->mic_muted = !!events[i].value;
            microphone_changed(state, TRUE);
            wake_from_privacy_switch();
        }
    }
    return G_SOURCE_CONTINUE;
}

/* --- ambient light ------------------------------------------------------ */

static gboolean on_light_event(gint fd, GIOCondition condition, gpointer data)
{
    struct state *state = data;
    struct input_event events[16];
    ssize_t size;
    size_t i;

    if (condition & (G_IO_ERR | G_IO_HUP)) {
        chiron_display_set_lux(state->display, 0, FALSE);
        return G_SOURCE_REMOVE;
    }
    size = read(fd, events, sizeof(events));
    if (size < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return G_SOURCE_CONTINUE;
        chiron_display_set_lux(state->display, 0, FALSE);
        return G_SOURCE_REMOVE;
    }
    for (i = 0; i < (size_t)size / sizeof(events[0]); i++) {
        if (events[i].type == EV_ABS && events[i].code == ABS_X)
            chiron_display_set_lux(state->display, events[i].value, TRUE);
    }
    return G_SOURCE_CONTINUE;
}

static int light_open(int *lux, gboolean *known)
{
    struct input_absinfo absinfo;
    char name[64];
    char path[64];
    int i;

    *lux = 0;
    *known = FALSE;
    for (i = 0; i < 32; i++) {
        int fd;

        g_snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0)
            continue;
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 ||
            g_strcmp0(name, ALS_NAME) != 0 ||
            ioctl(fd, EVIOCGABS(ABS_X), &absinfo) < 0) {
            close(fd);
            continue;
        }
        *lux = MAX(0, absinfo.value);
        *known = TRUE;
        return fd;
    }
    return -1;
}

static gboolean quit_main_loop(gpointer data)
{
    (void)data;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

/* --- start-up ----------------------------------------------------------- */

int main(int argc, char **argv)
{
    struct state state;
    GError *error = NULL;
    int switch_fd;
    int light_fd;
    int lux;
    int remote_status;
    gboolean lux_known;

    remote_status = chiron_display_remote_command(argc, argv);
    if (remote_status >= 0)
        return remote_status;

    gtk_init(&argc, &argv);
    memset(&state, 0, sizeof(state));

    /* The session bus is the same one the tray item goes out on; if it is
     * missing there is no tray either and nothing this process can do. */
    state.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!state.bus) {
        g_printerr("chiron-privacy: no session bus: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    switch_fd = open(SWITCH_DEVICE, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (switch_fd < 0) {
        g_printerr("chiron-privacy: cannot open %s: %s\n", SWITCH_DEVICE,
                   g_strerror(errno));
        return 1;
    }
    if (!switches_read(switch_fd, &state))
        return 1;

    light_fd = light_open(&lux, &lux_known);
    state.display = chiron_display_new(state.bus, lux, lux_known);

    /* The opening state is drawn but not announced -- a bubble on every
     * session start would report a switch nobody just touched. */
    privacy_changed(&state);

    if (light_fd >= 0)
        g_unix_fd_add(light_fd, G_IO_IN | G_IO_ERR | G_IO_HUP,
                      on_light_event, &state);
    else
        g_message("chiron-privacy: no ambient-light input device");

    g_unix_fd_add(switch_fd, G_IO_IN | G_IO_ERR | G_IO_HUP, on_switch_event,
                  &state);

    g_unix_signal_add(SIGTERM, quit_main_loop, NULL);
    g_unix_signal_add(SIGINT, quit_main_loop, NULL);

    gtk_main();
    return 0;
}
