// SPDX-License-Identifier: 0BSD
// Copyright (c) 2026 angellfear
#define _GNU_SOURCE

#include "chiron-display.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gtk-layer-shell.h>

#define BACKLIGHT_DIR "/sys/class/backlight/aml-bl"
#define DISPLAY_NAME "org.chiron.Display"
#define DISPLAY_PATH "/org/chiron/Display"
#define DISPLAY_IFACE "org.chiron.Display"
#define SNI_PATH "/StatusNotifierItem"
#define SNI_IFACE "org.kde.StatusNotifierItem"
#define SNI_WATCHER "org.kde.StatusNotifierWatcher"
#define LUX_MAX 20000.0
#define MAX_POINTS 16

/* The sensor is noisy and the curve is steep, so one reading is not a light
 * level: a hand passing over the board is enough to move the backlight. What
 * drives the curve is the mean of the last ten seconds, and the backlight is
 * then walked to it over three, which is slow enough that the eye reads it as
 * the room changing rather than as the screen doing something. */
#define LUX_WINDOW_US (10 * G_USEC_PER_SEC)
#define LUX_SAMPLES 128
#define RAMP_SECONDS 3.0

struct curve_point {
    double lux;
    double percent;
};

struct lux_sample {
    gint64 when_us;
    int lux;
};

struct display_config {
    gboolean use_sensor;
    gboolean smooth;
    int min_percent;
    int max_percent;
    int timeout_minutes;
    char timeout_action[8];
    char preset[16];
    char clock_font[80];
    char clock_language[4];
    int clock_size;
    gboolean night_mode;
    int night_threshold_lux;
    int night_release_lux;
    struct curve_point points[MAX_POINTS];
    int point_count;
};

struct chiron_display {
    GDBusConnection *bus;
    guint display_name_id;
    guint sni_name_id;
    guint watcher_id;
    guint sni_object_id;
    guint control_object_id;
    char *sni_name;
    gboolean sni_registered;
    int sni_attempts;

    struct display_config config;
    int lux;                /* the ten-second mean, not the last reading */
    gboolean lux_known;
    struct lux_sample lux_window[LUX_SAMPLES];
    int lux_count;
    int lux_next;
    gboolean night_active;
    gboolean camera_closed;
    gboolean mic_muted;
    gboolean screen_off;
    int applied_percent;
    guint ramp_source;
    int ramp_from;
    int ramp_to;
    gint64 ramp_started_us;

    GtkWidget *settings;
    GtkWidget *curve;
    GtkWidget *reading;
    GtkWidget *sensor_check;
    GtkWidget *preset_combo;
    GtkWidget *min_scale;
    GtkWidget *max_scale;
    GtkWidget *smooth_check;
    GtkWidget *timeout_combo;
    GtkWidget *action_combo;
    GtkWidget *timezone_combo;
    GtkWidget *font_button;
    GtkWidget *language_combo;
    GtkWidget *clock_size_scale;
    GtkWidget *night_check;
    GtkWidget *threshold_scale;
    GtkWidget *release_scale;
    int selected_point;
    int dragged_point;
    gboolean building_ui;

    GtkWidget *clock;
    GtkWidget *clock_box;
    guint clock_tick_source;
    gboolean clock_mapped;
    gboolean clock_hidden;
    GtkWidget *clock_time;
    GtkWidget *clock_date;
    GtkCssProvider *clock_css;
    GtkWidget *blanker;
};

static const struct curve_point balanced_curve[] = {
    {0, 5}, {100, 10}, {500, 25}, {1000, 35}, {2000, 50},
    {4000, 90}, {10000, 100}, {20000, 100},
};
static const struct curve_point night_curve[] = {
    {0, 2}, {10, 3}, {50, 5}, {200, 9}, {1000, 20},
    {4000, 45}, {20000, 70},
};
static const struct curve_point daylight_curve[] = {
    {0, 12}, {50, 18}, {200, 30}, {1000, 55}, {4000, 85},
    {10000, 100}, {20000, 100},
};

static const char sni_xml[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='AttentionIconName' type='s' access='read'/>"
    "<property name='OverlayIconName' type='s' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<method name='Activate'><arg name='x' type='i' direction='in'/>"
    "<arg name='y' type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg name='x' type='i' direction='in'/>"
    "<arg name='y' type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg name='x' type='i' direction='in'/>"
    "<arg name='y' type='i' direction='in'/></method>"
    "<method name='Scroll'><arg name='delta' type='i' direction='in'/>"
    "<arg name='orientation' type='s' direction='in'/></method>"
    "<signal name='NewIcon'/><signal name='NewToolTip'/>"
    "</interface></node>";

static const char control_xml[] =
    "<node><interface name='org.chiron.Display'>"
    "<method name='ShowSettings'/><method name='ShowClock'/>"
    "<method name='HideClock'/><method name='SetScreenOff'>"
    "<arg name='off' type='b' direction='in'/></method>"
    "</interface></node>";

static const char settings_css[] =
    "window.chiron-settings { background: #f2f5fa; color: #182033; }"
    "headerbar { min-height: 58px; background: #ffffff;"
    " border-bottom: 1px solid #dfe5ef; }"
    ".settings-page { padding: 24px 30px 34px 30px; }"
    ".card { background: #ffffff; border: 1px solid #dfe5ef;"
    " border-radius: 18px; padding: 22px; }"
    ".section-title { font-size: 20px; font-weight: 700; color: #172033; }"
    ".footer { font-size: 13px; color: #97a0b0; padding: 6px 14px 8px 14px; }"
    ".chooser { padding: 0 16px; }"
    ".chooser-list row { min-height: 64px; padding: 0 22px;"
    " border-bottom: 1px solid #eceff4; }"
    ".chooser-list row label { font-size: 17px; }"
    ".chooser-list scrollbar slider { min-width: 18px; border-radius: 9px; }"
    ".reading { font-size: 17px; font-weight: 600; color: #2265d8; }"
    "button, combobox, fontbutton { min-height: 46px; min-width: 52px; }"
    "checkbutton { min-height: 46px; }"
    "scale slider { min-width: 26px; min-height: 26px; }"
    "notebook header tabs { min-height: 48px; padding: 0 28px; }";

static double clamp_double(double value, double low, double high)
{
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

static int clamp_int(int value, int low, int high)
{
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

static double lux_position(double lux)
{
    return log10(1.0 + MAX(0.0, lux)) / log10(1.0 + LUX_MAX);
}

static double position_lux(double position)
{
    return pow(10.0, clamp_double(position, 0.0, 1.0) *
               log10(1.0 + LUX_MAX)) - 1.0;
}

static void set_curve(struct display_config *config,
                      const struct curve_point *points, int count,
                      const char *preset)
{
    config->point_count = MIN(count, MAX_POINTS);
    memcpy(config->points, points,
           (size_t)config->point_count * sizeof(config->points[0]));
    g_strlcpy(config->preset, preset, sizeof(config->preset));
}

static int compare_points(const void *left, const void *right)
{
    const struct curve_point *a = left;
    const struct curve_point *b = right;

    return (a->lux > b->lux) - (a->lux < b->lux);
}

static double curve_percent(const struct display_config *config, int lux)
{
    int i;

    if (config->point_count < 1)
        return 100.0;
    if (lux <= config->points[0].lux)
        return config->points[0].percent;
    for (i = 1; i < config->point_count; i++) {
        const struct curve_point *left = &config->points[i - 1];
        const struct curve_point *right = &config->points[i];

        if (lux <= right->lux) {
            double width = right->lux - left->lux;
            double ratio = width > 0 ? (lux - left->lux) / width : 0;
            return left->percent + ratio * (right->percent - left->percent);
        }
    }
    return config->points[config->point_count - 1].percent;
}

static char *config_path(const char *name)
{
    return g_build_filename(g_get_user_config_dir(), "chiron", name, NULL);
}

static gboolean screen_off_marker_exists(void)
{
    const char *runtime = g_get_user_runtime_dir();
    char *path;
    gboolean exists;

    if (!runtime)
        return FALSE;
    path = g_build_filename(runtime, "chiron-screen-off", NULL);
    exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return exists;
}

static void config_defaults(struct display_config *config)
{
    memset(config, 0, sizeof(*config));
    config->use_sensor = TRUE;
    config->smooth = TRUE;
    config->min_percent = 1;
    config->max_percent = 100;
    config->timeout_minutes = 1;
    g_strlcpy(config->timeout_action, "clock", sizeof(config->timeout_action));
    g_strlcpy(config->clock_font, "Sans", sizeof(config->clock_font));
    g_strlcpy(config->clock_language, "ru", sizeof(config->clock_language));
    config->clock_size = 220;
    config->night_mode = TRUE;
    config->night_threshold_lux = 30;
    config->night_release_lux = 50;
    set_curve(config, balanced_curve, G_N_ELEMENTS(balanced_curve), "balanced");
}

static void load_config(struct display_config *config)
{
    GKeyFile *key = g_key_file_new();
    GError *error = NULL;
    char *path = config_path("display.ini");
    char *curve;

    config_defaults(config);
    if (!g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, &error)) {
        g_clear_error(&error);
        goto out;
    }
#define READ_BOOL(field, keyname) do { \
    gboolean value = g_key_file_get_boolean(key, "display", keyname, &error); \
    if (!error) config->field = value; else g_clear_error(&error); \
} while (0)
#define READ_INT(field, keyname) do { \
    int value = g_key_file_get_integer(key, "display", keyname, &error); \
    if (!error) config->field = value; else g_clear_error(&error); \
} while (0)
    READ_BOOL(use_sensor, "use_sensor");
    READ_BOOL(smooth, "smooth");
    READ_INT(min_percent, "min_percent");
    READ_INT(max_percent, "max_percent");
    READ_INT(timeout_minutes, "timeout_minutes");
    READ_INT(clock_size, "clock_size");
    READ_BOOL(night_mode, "night_mode");
    READ_INT(night_threshold_lux, "night_threshold_lux");
    READ_INT(night_release_lux, "night_release_lux");
#undef READ_BOOL
#undef READ_INT
    {
        char *value = g_key_file_get_string(key, "display", "timeout_action", NULL);
        if (value) {
            g_strlcpy(config->timeout_action, value,
                      sizeof(config->timeout_action));
            g_free(value);
        }
        value = g_key_file_get_string(key, "display", "preset", NULL);
        if (value) {
            g_strlcpy(config->preset, value, sizeof(config->preset));
            g_free(value);
        }
        value = g_key_file_get_string(key, "display", "clock_font", NULL);
        if (value) {
            g_strlcpy(config->clock_font, value, sizeof(config->clock_font));
            g_free(value);
        }
        value = g_key_file_get_string(key, "display", "clock_language", NULL);
        if (value) {
            g_strlcpy(config->clock_language, value,
                      sizeof(config->clock_language));
            g_free(value);
        }
    }
    curve = g_key_file_get_string(key, "display", "curve", NULL);
    if (curve) {
        char **parts = g_strsplit(curve, ";", MAX_POINTS + 1);
        int count = 0;
        int i;

        for (i = 0; parts[i] && count < MAX_POINTS; i++) {
            double lux;
            double percent;
            char tail;

            if (sscanf(parts[i], "%lf:%lf%c", &lux, &percent, &tail) == 2) {
                config->points[count].lux = clamp_double(lux, 0, LUX_MAX);
                config->points[count].percent = clamp_double(percent, 0, 100);
                count++;
            }
        }
        if (count >= 2) {
            config->point_count = count;
            qsort(config->points, (size_t)count, sizeof(config->points[0]),
                  compare_points);
        }
        g_strfreev(parts);
        g_free(curve);
    }
out:
    config->min_percent = clamp_int(config->min_percent, 1, 60);
    config->max_percent = clamp_int(config->max_percent, 20, 100);
    if (config->min_percent > config->max_percent)
        config->min_percent = config->max_percent;
    config->timeout_minutes = clamp_int(config->timeout_minutes, 1, 240);
    config->clock_size = clamp_int(config->clock_size, 80, 360);
    config->night_threshold_lux =
        clamp_int(config->night_threshold_lux, 0, 500);
    config->night_release_lux = clamp_int(config->night_release_lux, 0, 600);
    /* The band only means anything upwards: leaving is always at least as
     * bright as entering, so a config that says otherwise collapses to none. */
    if (config->night_release_lux < config->night_threshold_lux)
        config->night_release_lux = config->night_threshold_lux;
    if (strcmp(config->timeout_action, "off") != 0 &&
        strcmp(config->timeout_action, "clock") != 0 &&
        strcmp(config->timeout_action, "none") != 0)
        g_strlcpy(config->timeout_action, "clock",
                  sizeof(config->timeout_action));
    if (strcmp(config->clock_language, "ru") != 0 &&
        strcmp(config->clock_language, "en") != 0)
        g_strlcpy(config->clock_language, "ru",
                  sizeof(config->clock_language));
    g_key_file_unref(key);
    g_free(path);
}

static void save_config(struct chiron_display *display)
{
    GKeyFile *key = g_key_file_new();
    GString *curve = g_string_new(NULL);
    char *path = config_path("display.ini");
    char *directory = g_path_get_dirname(path);
    char *data;
    gsize size;
    int i;

    g_mkdir_with_parents(directory, 0700);
    g_key_file_set_boolean(key, "display", "use_sensor",
                           display->config.use_sensor);
    g_key_file_set_boolean(key, "display", "smooth", display->config.smooth);
    g_key_file_set_integer(key, "display", "min_percent",
                           display->config.min_percent);
    g_key_file_set_integer(key, "display", "max_percent",
                           display->config.max_percent);
    g_key_file_set_integer(key, "display", "timeout_minutes",
                           display->config.timeout_minutes);
    g_key_file_set_string(key, "display", "timeout_action",
                          display->config.timeout_action);
    g_key_file_set_string(key, "display", "preset", display->config.preset);
    g_key_file_set_string(key, "display", "clock_font",
                          display->config.clock_font);
    g_key_file_set_string(key, "display", "clock_language",
                          display->config.clock_language);
    g_key_file_set_integer(key, "display", "clock_size",
                           display->config.clock_size);
    g_key_file_set_boolean(key, "display", "night_mode",
                           display->config.night_mode);
    g_key_file_set_integer(key, "display", "night_threshold_lux",
                           display->config.night_threshold_lux);
    g_key_file_set_integer(key, "display", "night_release_lux",
                           display->config.night_release_lux);
    for (i = 0; i < display->config.point_count; i++) {
        if (i)
            g_string_append_c(curve, ';');
        g_string_append_printf(curve, "%.0f:%.0f",
                               display->config.points[i].lux,
                               display->config.points[i].percent);
    }
    g_key_file_set_string(key, "display", "curve", curve->str);
    data = g_key_file_to_data(key, &size, NULL);
    if (!g_file_set_contents(path, data, (gssize)size, NULL))
        g_warning("chiron-privacy: cannot save %s", path);
    g_free(data);
    g_string_free(curve, TRUE);
    g_key_file_unref(key);
    g_free(directory);
    g_free(path);
}

static void signal_idle(void)
{
    GError *error = NULL;

    if (!g_spawn_command_line_async("/usr/local/bin/chiron-idle reload",
                                    &error)) {
        g_warning("chiron-display: cannot reload idle timer: %s",
                  error->message);
        g_error_free(error);
    }
}

static void save_power(struct chiron_display *display, gboolean reload)
{
    char *path = config_path("power.conf");
    char *directory = g_path_get_dirname(path);
    char *contents;

    g_mkdir_with_parents(directory, 0700);
    contents = g_strdup_printf(
        "TIMEOUT_MINUTES=%d\nTIMEOUT_ACTION=%s\nSUSPEND_MINUTES=0\n",
        display->config.timeout_minutes, display->config.timeout_action);
    if (!g_file_set_contents(path, contents, -1, NULL))
        g_warning("chiron-privacy: cannot save %s", path);
    if (reload)
        signal_idle();
    g_free(contents);
    g_free(directory);
    g_free(path);
}

static void emit_sni(struct chiron_display *display, const char *signal_name)
{
    g_dbus_connection_emit_signal(display->bus, NULL, SNI_PATH, SNI_IFACE,
                                  signal_name, NULL, NULL);
}

static void update_tray(struct chiron_display *display)
{
    emit_sni(display, "NewIcon");
    emit_sni(display, "NewAttentionIcon");
    emit_sni(display, "NewToolTip");
    g_dbus_connection_emit_signal(display->bus, NULL, SNI_PATH, SNI_IFACE,
        "NewStatus",
        g_variant_new("(s)", (display->camera_closed || display->mic_muted)
                                 ? "NeedsAttention" : "Active"), NULL);
}

/* One tray item for both hardware switches. Each of the four icons is a pair --
 * a camera glyph beside a microphone glyph, each in its live or struck-through
 * form -- so both switches are readable at once and neither state has to be
 * inferred from the absence of the other. The four files are composed from the
 * stock Adwaita glyphs and ship in hicolor. */
static const char *privacy_icon(const struct chiron_display *display)
{
    if (display->camera_closed && display->mic_muted)
        return "chiron-privacy-both-symbolic";
    if (display->camera_closed)
        return "chiron-privacy-camera-symbolic";
    if (display->mic_muted)
        return "chiron-privacy-microphone-symbolic";
    return "chiron-privacy-clear-symbolic";
}

static const char *privacy_text(const struct chiron_display *display)
{
    if (display->camera_closed && display->mic_muted)
        return "Камера закрыта, микрофон выключен";
    if (display->camera_closed)
        return "Камера закрыта";
    if (display->mic_muted)
        return "Микрофон выключен";
    return "Камера открыта, микрофон включён";
}

static void show_settings(struct chiron_display *display);
static void show_clock(struct chiron_display *display);
static void hide_clock(struct chiron_display *display);
static void set_screen_off(struct chiron_display *display, gboolean off);

static void sni_method_call(GDBusConnection *connection, const char *sender,
                            const char *object_path, const char *interface_name,
                            const char *method_name, GVariant *parameters,
                            GDBusMethodInvocation *invocation, gpointer data)
{
    struct chiron_display *display = data;

    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    if (strcmp(method_name, "Activate") == 0 ||
        strcmp(method_name, "SecondaryActivate") == 0 ||
        strcmp(method_name, "ContextMenu") == 0)
        show_settings(display);
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *sni_get_property(GDBusConnection *connection,
                                  const char *sender, const char *object_path,
                                  const char *interface_name,
                                  const char *property_name, GError **error,
                                  gpointer data)
{
    struct chiron_display *display = data;
    const char *icon = privacy_icon(display);

    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    if (strcmp(property_name, "Category") == 0)
        return g_variant_new_string("Hardware");
    if (strcmp(property_name, "Id") == 0)
        return g_variant_new_string("chiron-privacy");
    if (strcmp(property_name, "Title") == 0)
        return g_variant_new_string("Камера и микрофон");
    if (strcmp(property_name, "Status") == 0)
        return g_variant_new_string(
            (display->camera_closed || display->mic_muted) ? "NeedsAttention"
                                                           : "Active");
    if (strcmp(property_name, "IconName") == 0 ||
        strcmp(property_name, "AttentionIconName") == 0)
        return g_variant_new_string(icon);
    if (strcmp(property_name, "OverlayIconName") == 0)
        return g_variant_new_string("");
    if (strcmp(property_name, "ItemIsMenu") == 0)
        return g_variant_new_boolean(FALSE);
    if (strcmp(property_name, "ToolTip") == 0) {
        GVariantBuilder images;

        g_variant_builder_init(&images, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_new("(s@a(iiay)ss)", icon,
                             g_variant_builder_end(&images),
                             "Камера и микрофон", privacy_text(display));
    }
    return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
    sni_method_call, sni_get_property, NULL, {0},
};

static void control_method_call(GDBusConnection *connection, const char *sender,
                                const char *object_path,
                                const char *interface_name,
                                const char *method_name, GVariant *parameters,
                                GDBusMethodInvocation *invocation,
                                gpointer data)
{
    struct chiron_display *display = data;

    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    if (strcmp(method_name, "ShowSettings") == 0)
        show_settings(display);
    else if (strcmp(method_name, "ShowClock") == 0)
        show_clock(display);
    else if (strcmp(method_name, "HideClock") == 0)
        hide_clock(display);
    else if (strcmp(method_name, "SetScreenOff") == 0) {
        gboolean off;

        g_variant_get(parameters, "(b)", &off);
        set_screen_off(display, off);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable control_vtable = {
    control_method_call, NULL, NULL, {0},
};

static void register_sni(struct chiron_display *display);

static gboolean retry_sni(gpointer data)
{
    register_sni(data);
    return G_SOURCE_REMOVE;
}

static void register_sni_done(GObject *source, GAsyncResult *result,
                              gpointer data)
{
    struct chiron_display *display = data;
    GError *error = NULL;
    GVariant *reply;

    reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                          &error);
    if (reply) {
        display->sni_registered = TRUE;
        display->sni_attempts = 0;
        g_variant_unref(reply);
    } else {
        g_message("chiron-privacy: tray watcher not ready: %s", error->message);
        g_error_free(error);
        if (display->sni_attempts < 5)
            g_timeout_add_seconds((guint)(2 * display->sni_attempts),
                                  retry_sni, display);
    }
}

static void register_sni(struct chiron_display *display)
{
    if (!display->sni_name || display->sni_registered)
        return;
    display->sni_attempts++;
    g_dbus_connection_call(display->bus, SNI_WATCHER,
                           "/StatusNotifierWatcher", SNI_WATCHER,
                           "RegisterStatusNotifierItem",
                           g_variant_new("(s)", display->sni_name), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 4000, NULL,
                           register_sni_done, display);
}

static void sni_name_acquired(GDBusConnection *connection, const char *name,
                              gpointer data)
{
    struct chiron_display *display = data;

    (void)connection;
    (void)name;
    register_sni(display);
}

static void watcher_appeared(GDBusConnection *connection, const char *name,
                             const char *owner, gpointer data)
{
    (void)connection;
    (void)name;
    (void)owner;
    ((struct chiron_display *)data)->sni_attempts = 0;
    register_sni(data);
}

static void watcher_vanished(GDBusConnection *connection, const char *name,
                             gpointer data)
{
    struct chiron_display *display = data;

    (void)connection;
    (void)name;
    display->sni_registered = FALSE;
}

static int read_backlight_percent(void)
{
    char *maximum_text = NULL;
    char *brightness_text = NULL;
    int maximum;
    int brightness;
    int percent = -1;

    if (g_file_get_contents(BACKLIGHT_DIR "/max_brightness", &maximum_text,
                            NULL, NULL) &&
        g_file_get_contents(BACKLIGHT_DIR "/brightness", &brightness_text,
                            NULL, NULL) &&
        sscanf(maximum_text, "%d", &maximum) == 1 && maximum > 0 &&
        sscanf(brightness_text, "%d", &brightness) == 1)
        percent = clamp_int((int)lround(100.0 * brightness / maximum), 1, 100);
    g_free(maximum_text);
    g_free(brightness_text);
    return percent;
}

static gboolean write_backlight(int percent)
{
    char *maximum_text = NULL;
    char value_text[32];
    int maximum;
    int fd;
    int length;
    ssize_t written;

    if (!g_file_get_contents(BACKLIGHT_DIR "/max_brightness", &maximum_text,
                             NULL, NULL) ||
        sscanf(maximum_text, "%d", &maximum) != 1 || maximum < 1) {
        g_free(maximum_text);
        return FALSE;
    }
    /* Keep zero exclusively for chiron-idle's physical screen-off state.
     * The 1% UI floor is deliberately PWM 1 (not 3 on this 8-bit panel),
     * giving a usable near-black setting without blanking the panel. */
    length = g_snprintf(value_text, sizeof(value_text), "%d\n",
                        percent <= 1 ? 1 :
                        MAX(1, (int)lround(maximum * percent / 100.0)));
    g_free(maximum_text);
    fd = open(BACKLIGHT_DIR "/brightness", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return FALSE;
    written = write(fd, value_text, (size_t)length);
    close(fd);
    return written == (ssize_t)length;
}

static void refresh_reading_safe(struct chiron_display *display)
{
    char lux[40];
    char percent[40];
    char *text;

    if (!display->reading)
        return;
    if (display->lux_known)
        g_snprintf(lux, sizeof(lux), "%d lx", display->lux);
    else
        g_strlcpy(lux, "—", sizeof(lux));
    if (display->applied_percent >= 0)
        g_snprintf(percent, sizeof(percent), "%d%%", display->applied_percent);
    else
        g_strlcpy(percent, "—", sizeof(percent));
    text = g_strdup_printf("Освещённость: %s    •    Подсветка: %s",
                           lux, percent);
    gtk_label_set_text(GTK_LABEL(display->reading), text);
    g_free(text);
    gtk_widget_queue_draw(display->curve);
}

static gboolean ramp_step(gpointer data)
{
    struct chiron_display *display = data;
    double elapsed;
    double fraction;

    if (display->screen_off) {
        display->ramp_source = 0;
        return G_SOURCE_REMOVE;
    }
    elapsed = (g_get_monotonic_time() - display->ramp_started_us) / 1000000.0;
    fraction = MIN(1.0, elapsed / RAMP_SECONDS);
    {
        int next = (int)lround(display->ramp_from +
            (display->ramp_to - display->ramp_from) * fraction);
        if (write_backlight(next))
            display->applied_percent = next;
    }
    refresh_reading_safe(display);
    if (fraction >= 1.0) {
        display->ramp_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void start_brightness(struct chiron_display *display, int percent,
                             gboolean immediate)
{
    /* Already walking there: let it keep walking rather than restarting the
     * three seconds from the same place on every sensor reading. */
    if (display->ramp_source && !immediate && display->ramp_to == percent)
        return;
    if (display->ramp_source) {
        g_source_remove(display->ramp_source);
        display->ramp_source = 0;
    }
    if (immediate || !display->config.smooth ||
        display->applied_percent < 0 || display->applied_percent == percent) {
        if (write_backlight(percent))
            display->applied_percent = percent;
        refresh_reading_safe(display);
        return;
    }
    display->ramp_from = display->applied_percent;
    display->ramp_to = percent;
    display->ramp_started_us = g_get_monotonic_time();
    display->ramp_source = g_timeout_add(25, ramp_step, display);
}

/* No rate limit of its own any more: the ramp is the pacing. A new target
 * arriving mid-walk retargets from wherever the backlight has got to, so the
 * light tracks the room continuously instead of in two-second steps. */
static void request_brightness(struct chiron_display *display, int percent,
                               gboolean immediate)
{
    if (display->screen_off)
        return;
    start_brightness(display, clamp_int(percent, 1, 100), immediate);
}

static int target_brightness(struct chiron_display *display)
{
    return clamp_int((int)lround(curve_percent(&display->config, display->lux)),
                     display->config.min_percent,
                     display->config.max_percent);
}

static void apply_current_policy(struct chiron_display *display)
{
    if (display->config.use_sensor && display->lux_known)
        request_brightness(display, target_brightness(display), FALSE);
    else if (!display->config.use_sensor)
        request_brightness(display, display->config.max_percent, FALSE);
}

static void refresh_clock_style(struct chiron_display *display)
{
    gboolean night;
    char *font;
    char *css;

    if (!display->clock_css)
        return;
    /* Parked: the surface stays mapped but paints nothing. See clock_set_shown. */
    if (display->clock_hidden) {
        gtk_css_provider_load_from_data(display->clock_css,
            "#chiron-clock { background: rgba(0,0,0,0); }", -1, NULL);
        return;
    }
    /* Hysteresis. A single threshold puts the clock on the boundary of the
     * room's own noise and it flickers between red and white; red is entered
     * below the threshold and left only above the release level. */
    if (!display->config.night_mode || !display->lux_known)
        display->night_active = FALSE;
    else if (display->lux <= display->config.night_threshold_lux)
        display->night_active = TRUE;
    else if (display->lux >= display->config.night_release_lux)
        display->night_active = FALSE;
    night = display->night_active;
    font = g_markup_escape_text(display->config.clock_font, -1);
    css = g_strdup_printf(
        "#chiron-clock { background: #000000; }"
        "#clock-time, #clock-date { color: %s; font-family: '%s'; }"
        "#clock-time { font-size: %dpx; font-weight: 500; }"
        "#clock-date { font-size: %dpx; font-weight: 400; }",
        night ? "#b00000" : "#f7f8fb", font,
        display->config.clock_size, MAX(28, display->config.clock_size / 5));
    gtk_css_provider_load_from_data(display->clock_css, css, -1, NULL);
    g_free(css);
    g_free(font);
}

static gboolean clock_tick(gpointer data)
{
    struct chiron_display *display = data;
    GDateTime *now = g_date_time_new_now_local();
    char *time_text = g_date_time_format(now, "%H:%M");
    char *date_text;

    if (strcmp(display->config.clock_language, "ru") == 0) {
        static const char *weekdays[] = {
            "", "Понедельник", "Вторник", "Среда", "Четверг",
            "Пятница", "Суббота", "Воскресенье",
        };
        static const char *months[] = {
            "", "января", "февраля", "марта", "апреля", "мая",
            "июня", "июля", "августа", "сентября", "октября",
            "ноября", "декабря",
        };
        date_text = g_strdup_printf("%s, %d %s",
            weekdays[g_date_time_get_day_of_week(now)],
            g_date_time_get_day_of_month(now),
            months[g_date_time_get_month(now)]);
    } else {
        static const char *weekdays[] = {
            "", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
            "Saturday", "Sunday",
        };
        static const char *months[] = {
            "", "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December",
        };
        date_text = g_strdup_printf("%s, %d %s",
            weekdays[g_date_time_get_day_of_week(now)],
            g_date_time_get_day_of_month(now),
            months[g_date_time_get_month(now)]);
    }

    gtk_label_set_text(GTK_LABEL(display->clock_time), time_text);
    gtk_label_set_text(GTK_LABEL(display->clock_date), date_text);
    refresh_clock_style(display);
    g_free(date_text);
    g_free(time_text);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

static gboolean clock_press(GtkWidget *widget, GdkEventButton *event,
                            gpointer data)
{
    (void)widget;
    (void)event;
    hide_clock(data);
    return TRUE;
}

static gboolean clock_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget;
    if (event->keyval == GDK_KEY_Escape) {
        hide_clock(data);
        return TRUE;
    }
    return FALSE;
}

static void build_clock(struct chiron_display *display)
{
    GtkWidget *box;
    GtkStyleContext *context;

    display->clock = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(display->clock, "chiron-clock");
    /* An RGBA visual so the parked state can paint a genuinely transparent
     * frame rather than opaque black -- see clock_set_shown. */
    {
        GdkVisual *rgba =
            gdk_screen_get_rgba_visual(gtk_widget_get_screen(display->clock));

        if (rgba)
            gtk_widget_set_visual(display->clock, rgba);
    }
    gtk_window_set_decorated(GTK_WINDOW(display->clock), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(display->clock), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(display->clock), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(display->clock), TRUE);
    gtk_layer_init_for_window(GTK_WINDOW(display->clock));
    gtk_layer_set_namespace(GTK_WINDOW(display->clock), "chiron-clock");
    gtk_layer_set_layer(GTK_WINDOW(display->clock), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(display->clock), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(display->clock), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(display->clock), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(display->clock), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    /* Ignore panel/dock exclusive zones: fullscreen means the complete
     * physical output, including the strip reserved by wf-panel. */
    gtk_layer_set_exclusive_zone(GTK_WINDOW(display->clock), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(display->clock),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_widget_add_events(display->clock, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(display->clock, "button-press-event",
                     G_CALLBACK(clock_press), display);
    g_signal_connect(display->clock, "key-press-event",
                     G_CALLBACK(clock_key), display);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    display->clock_time = gtk_label_new("");
    display->clock_date = gtk_label_new("");
    gtk_widget_set_name(display->clock_time, "clock-time");
    gtk_widget_set_name(display->clock_date, "clock-date");
    gtk_box_pack_start(GTK_BOX(box), display->clock_time, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), display->clock_date, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(display->clock), box);
    display->clock_box = box;
    display->clock_css = gtk_css_provider_new();
    context = gtk_widget_get_style_context(display->clock);
    gtk_style_context_add_provider(context,
        GTK_STYLE_PROVIDER(display->clock_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    context = gtk_widget_get_style_context(display->clock_time);
    gtk_style_context_add_provider(context,
        GTK_STYLE_PROVIDER(display->clock_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    context = gtk_widget_get_style_context(display->clock_date);
    gtk_style_context_add_provider(context,
        GTK_STYLE_PROVIDER(display->clock_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    clock_tick(display);
}

/* The clock is one persistent layer surface for the whole session. It is mapped
 * once and never unmapped: "hiding" it repaints the fullscreen surface fully
 * transparent, with its contents hidden and its input region emptied, and
 * "showing" it puts the opaque clock back. Repeatedly mapping and unmapping a
 * fullscreen layer surface every idle cycle is what makes wayfire mis-deliver a
 * wl_buffer release to wf-panel, which then aborts in cairo_surface_reference;
 * a same-size repaint does not. chiron-panel is the backstop for when the panel
 * aborts anyway; this keeps chiron from provoking it. Anchors and layer are
 * left at their build-time values -- wayfire does not reliably honour changing
 * them after the surface is mapped. */
static void clock_set_shown(struct chiron_display *display, gboolean shown)
{
    GdkWindow *gdk_window = gtk_widget_get_window(display->clock);

    display->clock_hidden = !shown;

    if (shown) {
        gtk_widget_set_opacity(display->clock, 1.0);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(display->clock),
                                    GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        if (gdk_window)
            gdk_window_input_shape_combine_region(gdk_window, NULL, 0, 0);
        gtk_widget_show(display->clock_box);
    } else {
        cairo_region_t *nothing = cairo_region_create();

        /* Wayfire occasionally keeps the previous opaque layer buffer when a
         * GTK CSS background becomes transparent. Window opacity guarantees
         * that a resumed idle clock cannot leave a lit black screen behind. */
        gtk_widget_set_opacity(display->clock, 0.0);
        gtk_widget_hide(display->clock_box);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(display->clock),
                                    GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        if (gdk_window)
            gdk_window_input_shape_combine_region(gdk_window, nothing, 0, 0);
        cairo_region_destroy(nothing);
    }
    refresh_clock_style(display);
    gtk_widget_queue_draw(display->clock);
}

static void show_clock(struct chiron_display *display)
{
    if (display->screen_off)
        return;
    clock_tick(display);
    /* The per-second tick only runs while the clock is up. Left running for the
     * whole session it reloads the CSS provider on the layer-shell surface's
     * style context once a second behind a parked window, for nothing. */
    if (!display->clock_tick_source)
        display->clock_tick_source =
            g_timeout_add_seconds(1, clock_tick, display);
    if (!display->clock_mapped) {
        gtk_widget_show_all(display->clock);
        display->clock_mapped = TRUE;
    }
    clock_set_shown(display, TRUE);
    gtk_window_present(GTK_WINDOW(display->clock));
}

static void hide_clock(struct chiron_display *display)
{
    if (display->clock_tick_source) {
        g_source_remove(display->clock_tick_source);
        display->clock_tick_source = 0;
    }
    if (display->clock_mapped)
        clock_set_shown(display, FALSE);
}

static gboolean blanker_input(GtkWidget *widget, GdkEvent *event,
                              gpointer data)
{
    GError *error = NULL;

    (void)widget;
    (void)data;
    if (event->type != GDK_BUTTON_PRESS && event->type != GDK_TOUCH_BEGIN)
        return TRUE;
    /* Keep the overlay mapped until SetScreenOff(false) is received. This
     * consumes the complete first touch sequence instead of letting its
     * release fall through into the application below. */
    if (!g_spawn_command_line_async("/usr/local/bin/chiron-idle wake", &error)) {
        g_warning("chiron-display: cannot wake from touch: %s", error->message);
        g_error_free(error);
    }
    return TRUE;
}

static void build_blanker(struct chiron_display *display)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    GtkStyleContext *context;

    display->blanker = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(display->blanker, "chiron-blanker");
    gtk_window_set_decorated(GTK_WINDOW(display->blanker), FALSE);
    gtk_layer_init_for_window(GTK_WINDOW(display->blanker));
    gtk_layer_set_namespace(GTK_WINDOW(display->blanker), "chiron-blanker");
    gtk_layer_set_layer(GTK_WINDOW(display->blanker),
                        GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(display->blanker),
                         GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(display->blanker),
                         GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(display->blanker),
                         GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(display->blanker),
                         GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(display->blanker), -1);
    /* Volume keys must remain ordinary compositor/swayidle input. */
    gtk_layer_set_keyboard_mode(GTK_WINDOW(display->blanker),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_widget_add_events(display->blanker,
                          GDK_BUTTON_PRESS_MASK | GDK_TOUCH_MASK);
    g_signal_connect(display->blanker, "button-press-event",
                     G_CALLBACK(blanker_input), display);
    g_signal_connect(display->blanker, "touch-event",
                     G_CALLBACK(blanker_input), display);
    gtk_css_provider_load_from_data(
        provider, "#chiron-blanker { background: #000000; }", -1, NULL);
    context = gtk_widget_get_style_context(display->blanker);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void set_screen_off(struct chiron_display *display, gboolean off)
{
    if (display->screen_off == off)
        return;
    display->screen_off = off;
    if (off) {
        hide_clock(display);
        gtk_widget_show_all(display->blanker);
        if (display->ramp_source) {
            g_source_remove(display->ramp_source);
            display->ramp_source = 0;
        }
        return;
    }
    gtk_widget_hide(display->blanker);
    if (display->config.use_sensor && display->lux_known)
        request_brightness(display, target_brightness(display), TRUE);
}

/* A drop-down list, replaced.
 *
 * GtkComboBox opens a popup the width of its button and the height of its
 * list. On this screen that is a narrow column stranded in a window of empty
 * space, with rows sized for a mouse; the twenty-four timezones made it
 * plainly unusable. A chooser is a button that opens a modal list instead:
 * full width, one large row per choice, dismissed by picking one or by Cancel.
 *
 * It keeps the combo's shape -- an id and a label per entry, one active id --
 * so the code around it reads the same, and it calls its change handler only
 * when a person picks something, never while the window is being built. */
struct chooser {
    GtkWidget *label;
    char *title;
    GPtrArray *ids;
    GPtrArray *labels;
    char *active;
    struct chiron_display *display;
    void (*changed)(GtkWidget *, gpointer);
};

static void chooser_free(gpointer data)
{
    struct chooser *chooser = data;

    g_ptr_array_unref(chooser->ids);
    g_ptr_array_unref(chooser->labels);
    g_free(chooser->title);
    g_free(chooser->active);
    g_free(chooser);
}

static struct chooser *chooser_of(GtkWidget *button)
{
    return g_object_get_data(G_OBJECT(button), "chiron-chooser");
}

static const char *chooser_id(GtkWidget *button)
{
    return chooser_of(button)->active;
}

static void chooser_show_active(struct chooser *chooser)
{
    guint i;

    for (i = 0; i < chooser->ids->len; i++) {
        if (g_strcmp0(g_ptr_array_index(chooser->ids, i), chooser->active))
            continue;
        gtk_label_set_text(GTK_LABEL(chooser->label),
                           g_ptr_array_index(chooser->labels, i));
        return;
    }
    gtk_label_set_text(GTK_LABEL(chooser->label),
                       chooser->active ? chooser->active : "");
}

static gboolean chooser_set_id(GtkWidget *button, const char *id)
{
    struct chooser *chooser = chooser_of(button);
    guint i;

    for (i = 0; i < chooser->ids->len; i++) {
        if (g_strcmp0(g_ptr_array_index(chooser->ids, i), id))
            continue;
        g_free(chooser->active);
        chooser->active = g_strdup(id);
        chooser_show_active(chooser);
        return TRUE;
    }
    return FALSE;
}

static void chooser_add(GtkWidget *button, const char *id, const char *label)
{
    struct chooser *chooser = chooser_of(button);

    g_ptr_array_add(chooser->ids, g_strdup(id));
    g_ptr_array_add(chooser->labels, g_strdup(label));
    if (!chooser->active)
        chooser_set_id(button, id);
}

static void chooser_clear(GtkWidget *button)
{
    struct chooser *chooser = chooser_of(button);

    g_ptr_array_set_size(chooser->ids, 0);
    g_ptr_array_set_size(chooser->labels, 0);
    g_free(chooser->active);
    chooser->active = NULL;
    gtk_label_set_text(GTK_LABEL(chooser->label), "");
}

static void chooser_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)response;
    (void)data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void chooser_row_activated(GtkListBox *list, GtkListBoxRow *row,
                                  gpointer data)
{
    GtkWidget *button = data;
    struct chooser *chooser = chooser_of(button);
    int index = gtk_list_box_row_get_index(row);
    gboolean picked = FALSE;

    if (index >= 0 && (guint)index < chooser->ids->len)
        picked = chooser_set_id(button,
                                g_ptr_array_index(chooser->ids, index));
    gtk_widget_destroy(gtk_widget_get_toplevel(GTK_WIDGET(list)));
    if (picked && chooser->changed)
        chooser->changed(button, chooser->display);
}

static void chooser_clicked(GtkButton *button, gpointer data)
{
    struct chooser *chooser = chooser_of(GTK_WIDGET(button));
    GtkWidget *dialog;
    GtkWidget *list;
    GtkWidget *scroller;
    GtkWidget *selected = NULL;
    guint i;

    (void)data;
    dialog = gtk_dialog_new_with_buttons(
        chooser->title,
        chooser->display->settings ? GTK_WINDOW(chooser->display->settings)
                                   : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Отмена", GTK_RESPONSE_CANCEL, NULL);
    /* The width is the window's, not the list's: a chooser that shrinks to
     * the longest label lands somewhere different for every selector, and two
     * short entries would open a sliver. Height is left at -1 so it still
     * follows the content. */
    gtk_window_set_default_size(GTK_WINDOW(dialog), 760, -1);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog),
                                "chiron-settings");
    list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(list),
                                "chooser-list");
    for (i = 0; i < chooser->ids->len; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(g_ptr_array_index(chooser->labels, i));

        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_container_add(GTK_CONTAINER(row), label);
        gtk_container_add(GTK_CONTAINER(list), row);
        if (g_strcmp0(g_ptr_array_index(chooser->ids, i), chooser->active) == 0)
            selected = row;
    }
    g_signal_connect(list, "row-activated", G_CALLBACK(chooser_row_activated),
                     button);
    scroller = gtk_scrolled_window_new(NULL, NULL);
    /* Two entries should not open a window with room for twenty-four. The
     * scroller reports what it holds and the window follows, up to a cap that
     * keeps the timezone list on one screen. Overlay scrolling is off, so the
     * bar takes real space and is there to be seen the moment the list is
     * longer than the window -- and AUTOMATIC, so a list that fits does not
     * carry a scrollbar with nowhere to go. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroller),
                                              FALSE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller),
                                               780);
    gtk_container_add(GTK_CONTAINER(scroller), list);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_container_add(
        GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
        scroller);
    g_signal_connect(dialog, "response", G_CALLBACK(chooser_response), NULL);
    gtk_widget_show_all(dialog);
    if (selected) {
        gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(selected));
        gtk_widget_grab_focus(selected);
    }
}

static GtkWidget *chooser_new(struct chiron_display *display, const char *title,
                              void (*changed)(GtkWidget *, gpointer))
{
    GtkWidget *button = gtk_button_new();
    struct chooser *chooser = g_new0(struct chooser, 1);

    chooser->label = gtk_label_new("");
    chooser->title = g_strdup(title);
    chooser->ids = g_ptr_array_new_with_free_func(g_free);
    chooser->labels = g_ptr_array_new_with_free_func(g_free);
    chooser->display = display;
    chooser->changed = changed;
    gtk_label_set_xalign(GTK_LABEL(chooser->label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(chooser->label), PANGO_ELLIPSIZE_END);
    gtk_container_add(GTK_CONTAINER(button), chooser->label);
    gtk_widget_set_hexpand(button, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(button),
                                "chooser");
    g_object_set_data_full(G_OBJECT(button), "chiron-chooser", chooser,
                           chooser_free);
    g_signal_connect(button, "clicked", G_CALLBACK(chooser_clicked), NULL);
    return button;
}

static void curve_geometry(GtkWidget *widget, double *x, double *y,
                           double *width, double *height)
{
    GtkAllocation allocation;

    gtk_widget_get_allocation(widget, &allocation);
    *x = 58;
    *y = 18;
    *width = MAX(1, allocation.width - 82);
    *height = MAX(1, allocation.height - 58);
}

static void point_pixels(GtkWidget *widget, const struct curve_point *point,
                         double *x, double *y)
{
    double x0;
    double y0;
    double width;
    double height;

    curve_geometry(widget, &x0, &y0, &width, &height);
    *x = x0 + lux_position(point->lux) * width;
    *y = y0 + (1.0 - point->percent / 100.0) * height;
}

static int curve_hit(struct chiron_display *display, double px, double py)
{
    int i;

    for (i = 0; i < display->config.point_count; i++) {
        double x;
        double y;

        point_pixels(display->curve, &display->config.points[i], &x, &y);
        if ((x - px) * (x - px) + (y - py) * (y - py) <= 22 * 22)
            return i;
    }
    return -1;
}

static struct curve_point curve_from_pixels(GtkWidget *widget, double px,
                                             double py)
{
    double x;
    double y;
    double width;
    double height;
    struct curve_point point;

    curve_geometry(widget, &x, &y, &width, &height);
    point.lux = lround(position_lux((px - x) / width));
    point.percent = lround(clamp_double((1.0 - (py - y) / height) * 100,
                                       0, 100));
    return point;
}

static void curve_changed(struct chiron_display *display)
{
    g_strlcpy(display->config.preset, "custom",
              sizeof(display->config.preset));
    if (display->preset_combo)
        chooser_set_id(display->preset_combo, "custom");
    save_config(display);
    apply_current_policy(display);
    gtk_widget_queue_draw(display->curve);
}

static gboolean curve_button(GtkWidget *widget, GdkEventButton *event,
                             gpointer data)
{
    struct chiron_display *display = data;
    int hit = curve_hit(display, event->x, event->y);
    double x;
    double y;
    double width;
    double height;

    curve_geometry(widget, &x, &y, &width, &height);
    if (hit < 0 && display->config.point_count < MAX_POINTS &&
        event->x >= x && event->x <= x + width && event->y >= y &&
        event->y <= y + height) {
        display->config.points[display->config.point_count++] =
            curve_from_pixels(widget, event->x, event->y);
        qsort(display->config.points, (size_t)display->config.point_count,
              sizeof(display->config.points[0]), compare_points);
        hit = curve_hit(display, event->x, event->y);
        curve_changed(display);
    }
    display->selected_point = hit;
    display->dragged_point = hit;
    gtk_widget_queue_draw(widget);
    return TRUE;
}

static gboolean curve_motion(GtkWidget *widget, GdkEventMotion *event,
                             gpointer data)
{
    struct chiron_display *display = data;
    struct curve_point moved;

    if (display->dragged_point < 0)
        return FALSE;
    moved = curve_from_pixels(widget, event->x, event->y);
    display->config.points[display->dragged_point] = moved;
    qsort(display->config.points, (size_t)display->config.point_count,
          sizeof(display->config.points[0]), compare_points);
    display->dragged_point = curve_hit(display, event->x, event->y);
    display->selected_point = display->dragged_point;
    curve_changed(display);
    return TRUE;
}

static gboolean curve_release(GtkWidget *widget, GdkEventButton *event,
                              gpointer data)
{
    struct chiron_display *display = data;

    (void)widget;
    (void)event;
    display->dragged_point = -1;
    return TRUE;
}

static gboolean curve_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    struct chiron_display *display = data;
    double x0;
    double y0;
    double width;
    double height;
    int i;

    curve_geometry(widget, &x0, &y0, &width, &height);
    cairo_set_source_rgb(cr, 0.965, 0.975, 0.995);
    cairo_rectangle(cr, x0, y0, width, height);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0.15, 0.22, 0.34, 0.14);
    {
        const int lux_lines[] = {0, 10, 100, 1000, 10000, 20000};
        const int percent_lines[] = {0, 25, 50, 75, 100};
        for (i = 0; i < (int)G_N_ELEMENTS(lux_lines); i++) {
            double x = x0 + lux_position(lux_lines[i]) * width;
            cairo_move_to(cr, x, y0);
            cairo_line_to(cr, x, y0 + height);
        }
        for (i = 0; i < (int)G_N_ELEMENTS(percent_lines); i++) {
            double y = y0 + (1.0 - percent_lines[i] / 100.0) * height;
            cairo_move_to(cr, x0, y);
            cairo_line_to(cr, x0 + width, y);
        }
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, 0.35, 0.40, 0.50);
        cairo_set_font_size(cr, 13);
        for (i = 0; i < (int)G_N_ELEMENTS(lux_lines); i++) {
            char label[16];
            double x = x0 + lux_position(lux_lines[i]) * width;

            if (lux_lines[i] >= 1000)
                g_snprintf(label, sizeof(label), "%dk", lux_lines[i] / 1000);
            else
                g_snprintf(label, sizeof(label), "%d", lux_lines[i]);
            cairo_move_to(cr, x - 8, y0 + height + 25);
            cairo_show_text(cr, label);
        }
        for (i = 0; i < (int)G_N_ELEMENTS(percent_lines); i++) {
            char label[16];
            double y = y0 + (1.0 - percent_lines[i] / 100.0) * height;

            g_snprintf(label, sizeof(label), "%d%%", percent_lines[i]);
            cairo_move_to(cr, 12, y + 5);
            cairo_show_text(cr, label);
        }
    }
    cairo_set_line_width(cr, 4);
    cairo_set_source_rgb(cr, 0.16, 0.43, 0.93);
    for (i = 0; i < display->config.point_count; i++) {
        double x;
        double y;

        point_pixels(widget, &display->config.points[i], &x, &y);
        if (i == 0)
            cairo_move_to(cr, x, y);
        else
            cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
    for (i = 0; i < display->config.point_count; i++) {
        double x;
        double y;

        point_pixels(widget, &display->config.points[i], &x, &y);
        cairo_set_source_rgb(cr, 0.16, 0.43, 0.93);
        cairo_arc(cr, x, y, i == display->selected_point ? 12 : 9,
                  0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_arc(cr, x, y, 4, 0, 2 * G_PI);
        cairo_fill(cr);
    }
    if (display->lux_known) {
        double x = x0 + lux_position(display->lux) * width;
        double y = y0 + (1.0 - display->applied_percent / 100.0) * height;

        cairo_set_source_rgb(cr, 0.08, 0.68, 0.38);
        cairo_set_line_width(cr, 2);
        cairo_move_to(cr, x, y0);
        cairo_line_to(cr, x, y0 + height);
        cairo_stroke(cr);
        if (display->applied_percent >= 0) {
            cairo_arc(cr, x, y, 7, 0, 2 * G_PI);
            cairo_fill(cr);
        }
    }
    return FALSE;
}

static GtkWidget *make_card(const char *title)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget *heading = gtk_label_new(title);

    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading),
                                "section-title");
    gtk_box_pack_start(GTK_BOX(card), heading, FALSE, FALSE, 0);
    return card;
}

/* Two of the three cards moved onto one page, which can now be taller than
 * the window on a short screen; vertical scrolling is what keeps the last card
 * reachable instead of cropped. */
static GtkWidget *make_page(GtkWidget **content_out)
{
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    GtkWidget *page = gtk_scrolled_window_new(NULL, NULL);

    gtk_container_set_border_width(GTK_CONTAINER(content), 24);
    gtk_style_context_add_class(gtk_widget_get_style_context(content),
                                "settings-page");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(page), content);
    *content_out = content;
    return page;
}

/* A strip of two equally wide labelled controls. Each cell lands just under
 * half the card, which is what puts two of them on one line on this screen. */
static GtkWidget *make_strip(void)
{
    GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);

    gtk_box_set_homogeneous(GTK_BOX(strip), TRUE);
    return strip;
}

static GtkWidget *strip_cell(GtkWidget *strip, const char *caption)
{
    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *label = gtk_label_new(caption);

    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(cell), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(strip), cell, TRUE, TRUE, 0);
    return cell;
}

static GtkWidget *make_scale(double low, double high, double value)
{
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                low, high, 1);

    gtk_range_set_value(GTK_RANGE(scale), value);
    gtk_scale_set_digits(GTK_SCALE(scale), 0);
    gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
    gtk_widget_set_hexpand(scale, TRUE);
    return scale;
}

static void sensor_toggled(GtkToggleButton *button, gpointer data)
{
    struct chiron_display *display = data;

    if (display->building_ui)
        return;
    display->config.use_sensor = gtk_toggle_button_get_active(button);
    save_config(display);
    apply_current_policy(display);
}

static void preset_changed(GtkWidget *widget, gpointer data)
{
    struct chiron_display *display = data;
    const char *id;

    if (display->building_ui)
        return;
    id = chooser_id(widget);
    if (!id || strcmp(id, "custom") == 0)
        return;
    if (strcmp(id, "night") == 0)
        set_curve(&display->config, night_curve, G_N_ELEMENTS(night_curve), id);
    else if (strcmp(id, "balanced") == 0)
        set_curve(&display->config, balanced_curve,
                  G_N_ELEMENTS(balanced_curve), id);
    else if (strcmp(id, "daylight") == 0)
        set_curve(&display->config, daylight_curve,
                  G_N_ELEMENTS(daylight_curve), id);
    save_config(display);
    apply_current_policy(display);
    gtk_widget_queue_draw(display->curve);
}

static void delete_point(GtkButton *button, gpointer data)
{
    struct chiron_display *display = data;

    (void)button;
    if (display->selected_point < 0 || display->config.point_count <= 2)
        return;
    memmove(&display->config.points[display->selected_point],
            &display->config.points[display->selected_point + 1],
            (size_t)(display->config.point_count - display->selected_point - 1) *
            sizeof(display->config.points[0]));
    display->config.point_count--;
    display->selected_point = -1;
    curve_changed(display);
}

static void limits_changed(GtkWidget *widget, gpointer data)
{
    struct chiron_display *display = data;

    (void)widget;
    if (display->building_ui)
        return;
    display->config.min_percent =
        (int)gtk_range_get_value(GTK_RANGE(display->min_scale));
    display->config.max_percent =
        (int)gtk_range_get_value(GTK_RANGE(display->max_scale));
    if (display->config.min_percent > display->config.max_percent) {
        display->config.min_percent = display->config.max_percent;
        gtk_range_set_value(GTK_RANGE(display->min_scale),
                            display->config.min_percent);
    }
    display->config.smooth =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(display->smooth_check));
    save_config(display);
    apply_current_policy(display);
}

static char *system_timezone(GError **error)
{
    GDBusProxy *proxy;
    GVariant *value;
    char *timezone = NULL;

    proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.timedate1", "/org/freedesktop/timedate1",
        "org.freedesktop.timedate1", NULL, error);
    if (!proxy)
        return NULL;
    value = g_dbus_proxy_get_cached_property(proxy, "Timezone");
    if (value) {
        timezone = g_variant_dup_string(value, NULL);
        g_variant_unref(value);
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Служба системного времени не вернула часовой пояс");
    }
    g_object_unref(proxy);
    return timezone;
}

static gboolean timezone_is_available(const char *timezone)
{
    char *path;
    gboolean available;

    if (!timezone || !*timezone || timezone[0] == '/' ||
        strstr(timezone, ".."))
        return FALSE;
    path = g_build_filename("/usr/share/zoneinfo", timezone, NULL);
    available = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return available;
}

static void timezone_error_response(GtkDialog *dialog, gint response,
                                    gpointer user_data)
{
    (void)response;
    (void)user_data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void show_timezone_error(struct chiron_display *display, GError *error)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        display->settings ? GTK_WINDOW(display->settings) : NULL,
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "Не удалось изменить системный часовой пояс");

    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                             error ? error->message : "Неизвестная ошибка");
    g_signal_connect(dialog, "response", G_CALLBACK(timezone_error_response), NULL);
    gtk_widget_show_all(dialog);
}

static gboolean set_system_timezone(const char *timezone, GError **error)
{
    GDBusProxy *proxy;
    GVariant *reply;

    if (!timezone_is_available(timezone)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Недопустимый часовой пояс: %s", timezone ? timezone : "");
        return FALSE;
    }
    proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.timedate1", "/org/freedesktop/timedate1",
        "org.freedesktop.timedate1", NULL, error);
    if (!proxy)
        return FALSE;
    /* No timeout. The call is allowed to raise a polkit prompt, and the
     * default twenty-five seconds is measured against a person finding the
     * on-screen keyboard and typing on it. That expires mid-typing, and the
     * change then fails as "Timeout was reached" even when what was typed was
     * right. The dialog has its own Cancel, which is what ends this call. */
    reply = g_dbus_proxy_call_sync(proxy, "SetTimezone",
                                   g_variant_new("(sb)", timezone, TRUE),
                                   G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                                   G_MAXINT, NULL, error);
    g_object_unref(proxy);
    if (!reply)
        return FALSE;
    g_variant_unref(reply);
    return TRUE;
}

/* One entry per hour of UTC offset, each named by the IANA zone conventionally
 * used to stand for that offset. The full zone.tab is four hundred entries and
 * needs a region step to be navigable at all; this board is a clock on a wall,
 * and the offset is the whole of what it has to be told. */
static const struct {
    const char *name;
    const char *label;
} timezone_table[] = {
    { "Pacific/Midway",         "UTC-11 — Pacific/Midway" },
    { "Pacific/Honolulu",       "UTC-10 — Pacific/Honolulu" },
    { "America/Anchorage",      "UTC-09 — America/Anchorage" },
    { "America/Los_Angeles",    "UTC-08 — America/Los_Angeles" },
    { "America/Denver",         "UTC-07 — America/Denver" },
    { "America/Chicago",        "UTC-06 — America/Chicago" },
    { "America/New_York",       "UTC-05 — America/New_York" },
    { "America/Halifax",        "UTC-04 — America/Halifax" },
    { "America/Sao_Paulo",      "UTC-03 — America/Sao_Paulo" },
    { "Atlantic/South_Georgia", "UTC-02 — Atlantic/South_Georgia" },
    { "Atlantic/Azores",        "UTC-01 — Atlantic/Azores" },
    { "Europe/London",          "UTC+00 — Europe/London" },
    { "Europe/Berlin",          "UTC+01 — Europe/Berlin" },
    { "Europe/Kyiv",            "UTC+02 — Europe/Kyiv" },
    { "Europe/Moscow",          "UTC+03 — Europe/Moscow" },
    { "Asia/Dubai",             "UTC+04 — Asia/Dubai" },
    { "Asia/Karachi",           "UTC+05 — Asia/Karachi" },
    { "Asia/Almaty",            "UTC+06 — Asia/Almaty" },
    { "Asia/Bangkok",           "UTC+07 — Asia/Bangkok" },
    { "Asia/Shanghai",          "UTC+08 — Asia/Shanghai" },
    { "Asia/Tokyo",             "UTC+09 — Asia/Tokyo" },
    { "Australia/Brisbane",     "UTC+10 — Australia/Brisbane" },
    { "Pacific/Guadalcanal",    "UTC+11 — Pacific/Guadalcanal" },
    { "Pacific/Auckland",       "UTC+12 — Pacific/Auckland" },
};

static void populate_timezones(struct chiron_display *display,
                               const char *current)
{
    gboolean listed = FALSE;
    size_t i;

    chooser_clear(display->timezone_combo);
    for (i = 0; i < G_N_ELEMENTS(timezone_table); i++) {
        if (!timezone_is_available(timezone_table[i].name))
            continue;
        chooser_add(display->timezone_combo, timezone_table[i].name,
                    timezone_table[i].label);
        if (g_strcmp0(timezone_table[i].name, current) == 0)
            listed = TRUE;
    }
    /* A system already set to a zone the table does not name -- Europe/Samara,
     * say -- has to see its own setting rather than a nearby lie, so that zone
     * joins the list instead of being quietly replaced. */
    if (!listed && timezone_is_available(current))
        chooser_add(display->timezone_combo, current, current);
    if (current)
        chooser_set_id(display->timezone_combo, current);
}

static void timezone_changed(GtkWidget *widget, gpointer data)
{
    struct chiron_display *display = data;
    const char *timezone;
    GError *error = NULL;
    char *current;

    (void)widget;
    if (display->building_ui)
        return;
    timezone = chooser_id(display->timezone_combo);
    if (!timezone)
        return;
    if (set_system_timezone(timezone, &error)) {
        tzset();
        clock_tick(display);
        return;
    }
    show_timezone_error(display, error);
    g_clear_error(&error);
    current = system_timezone(NULL);
    display->building_ui = TRUE;
    populate_timezones(display, current);
    display->building_ui = FALSE;
    g_free(current);
}

static void power_changed(GtkWidget *widget, gpointer data)
{
    struct chiron_display *display = data;
    const char *minutes;
    const char *action;

    (void)widget;
    if (display->building_ui)
        return;
    minutes = chooser_id(display->timeout_combo);
    action = chooser_id(display->action_combo);
    if (minutes)
        display->config.timeout_minutes = atoi(minutes);
    if (action)
        g_strlcpy(display->config.timeout_action, action,
                  sizeof(display->config.timeout_action));
    save_config(display);
    save_power(display, TRUE);
}

static void clock_settings_changed(GtkWidget *widget, gpointer data)
{
    struct chiron_display *display = data;
    char *font;
    PangoFontDescription *description;

    (void)widget;
    if (display->building_ui)
        return;
    font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(display->font_button));
    description = pango_font_description_from_string(font);
    if (pango_font_description_get_family(description))
        g_strlcpy(display->config.clock_font,
                  pango_font_description_get_family(description),
                  sizeof(display->config.clock_font));
    pango_font_description_free(description);
    g_free(font);
    {
        const char *language = chooser_id(display->language_combo);
        if (language)
            g_strlcpy(display->config.clock_language, language,
                      sizeof(display->config.clock_language));
    }
    display->config.clock_size =
        (int)gtk_range_get_value(GTK_RANGE(display->clock_size_scale));
    display->config.night_mode =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(display->night_check));
    display->config.night_threshold_lux =
        (int)gtk_range_get_value(GTK_RANGE(display->threshold_scale));
    display->config.night_release_lux =
        (int)gtk_range_get_value(GTK_RANGE(display->release_scale));
    /* Leaving night mode below the level that enters it is not a band but a
     * flicker, so the release level is pushed up rather than accepted. */
    if (display->config.night_release_lux < display->config.night_threshold_lux) {
        display->config.night_release_lux = display->config.night_threshold_lux;
        display->building_ui = TRUE;
        gtk_range_set_value(GTK_RANGE(display->release_scale),
                            display->config.night_release_lux);
        display->building_ui = FALSE;
    }
    save_config(display);
    refresh_clock_style(display);
}

static void preview_clock(GtkButton *button, gpointer data)
{
    (void)button;
    show_clock(data);
}

static gboolean settings_delete(GtkWidget *widget, GdkEvent *event,
                                gpointer data)
{
    (void)event;
    (void)data;
    gtk_widget_hide(widget);
    return TRUE;
}

static GtkWidget *build_brightness_page(struct chiron_display *display)
{
    GtkWidget *content;
    GtkWidget *page = make_page(&content);
    GtkWidget *card = make_card("Автоматическая яркость");
    GtkWidget *row;
    GtkWidget *label;
    GtkWidget *button;
    GtkWidget *grid;

    display->sensor_check =
        gtk_check_button_new_with_label("Использовать датчик освещённости");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(display->sensor_check),
                                 display->config.use_sensor);
    g_signal_connect(display->sensor_check, "toggled",
                     G_CALLBACK(sensor_toggled), display);
    gtk_box_pack_start(GTK_BOX(card), display->sensor_check, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    label = gtk_label_new("Профиль");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    display->preset_combo = chooser_new(display, "Профиль", preset_changed);
    chooser_add(display->preset_combo, "night", "Мягкий вечер");
    chooser_add(display->preset_combo, "balanced", "Сбалансированный");
    chooser_add(display->preset_combo, "daylight", "Яркий день");
    chooser_add(display->preset_combo, "custom", "Пользовательский");
    chooser_set_id(display->preset_combo, display->config.preset);
    gtk_box_pack_start(GTK_BOX(row), display->preset_combo, TRUE, TRUE, 0);
    button = gtk_button_new_with_label("Удалить выбранную точку");
    g_signal_connect(button, "clicked", G_CALLBACK(delete_point), display);
    gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), row, FALSE, FALSE, 0);
    display->curve = gtk_drawing_area_new();
    gtk_widget_set_size_request(display->curve, -1, 330);
    gtk_widget_add_events(display->curve, GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(display->curve, "draw", G_CALLBACK(curve_draw), display);
    g_signal_connect(display->curve, "button-press-event",
                     G_CALLBACK(curve_button), display);
    g_signal_connect(display->curve, "button-release-event",
                     G_CALLBACK(curve_release), display);
    g_signal_connect(display->curve, "motion-notify-event",
                     G_CALLBACK(curve_motion), display);
    gtk_box_pack_start(GTK_BOX(card), display->curve, TRUE, TRUE, 0);
    display->reading = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(display->reading), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(display->reading),
                                "reading");
    gtk_box_pack_start(GTK_BOX(card), display->reading, FALSE, FALSE, 0);
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    label = gtk_label_new("Минимум яркости");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    display->min_scale = make_scale(1, 60, display->config.min_percent);
    gtk_grid_attach(GTK_GRID(grid), display->min_scale, 1, 0, 1, 1);
    label = gtk_label_new("Максимум яркости");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
    display->max_scale = make_scale(20, 100, display->config.max_percent);
    gtk_grid_attach(GTK_GRID(grid), display->max_scale, 1, 1, 1, 1);
    display->smooth_check =
        gtk_check_button_new_with_label("Плавно менять яркость");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(display->smooth_check),
                                 display->config.smooth);
    gtk_grid_attach(GTK_GRID(grid), display->smooth_check, 0, 2, 2, 1);
    g_signal_connect(display->min_scale, "value-changed",
                     G_CALLBACK(limits_changed), display);
    g_signal_connect(display->max_scale, "value-changed",
                     G_CALLBACK(limits_changed), display);
    g_signal_connect(display->smooth_check, "toggled",
                     G_CALLBACK(limits_changed), display);
    gtk_box_pack_start(GTK_BOX(card), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), card, TRUE, TRUE, 0);
    return page;
}

/* Idle behaviour, system time and the fullscreen clock are one subject -- what
 * the screen shows when nobody is touching it -- and were split over two tabs
 * only because each was written on its own day. They share a page. */
static GtkWidget *build_screen_page(struct chiron_display *display)
{
    GtkWidget *content;
    GtkWidget *page = make_page(&content);
    GtkWidget *idle = make_card("Действие при бездействии");
    GtkWidget *time = make_card("Системное время");
    GtkWidget *clock = make_card("Полноэкранные часы");
    GtkWidget *strip;
    GtkWidget *cell;
    GtkWidget *row;
    GtkWidget *label;
    GtkWidget *button;
    char *timezone;
    GError *timezone_error = NULL;

    strip = make_strip();
    cell = strip_cell(strip, "Через");
    display->timeout_combo = chooser_new(display, "Через", power_changed);
#define TIMEOUT(id, text) chooser_add(display->timeout_combo, id, text)
    TIMEOUT("1", "1 минуту");
    TIMEOUT("2", "2 минуты");
    TIMEOUT("5", "5 минут");
    TIMEOUT("10", "10 минут");
    TIMEOUT("15", "15 минут");
    TIMEOUT("30", "30 минут");
    TIMEOUT("60", "1 час");
#undef TIMEOUT
    {
        char minutes[16];

        g_snprintf(minutes, sizeof(minutes), "%d",
                   display->config.timeout_minutes);
        if (!chooser_set_id(display->timeout_combo, minutes))
            chooser_set_id(display->timeout_combo, "1");
    }
    gtk_box_pack_start(GTK_BOX(cell), display->timeout_combo, TRUE, TRUE, 0);
    cell = strip_cell(strip, "После");
    display->action_combo = chooser_new(display, "После таймаута",
                                       power_changed);
    chooser_add(display->action_combo, "off", "Погасить экран");
    chooser_add(display->action_combo, "clock", "Показать часы");
    chooser_add(display->action_combo, "none", "Оставить экран включённым");
    chooser_set_id(display->action_combo, display->config.timeout_action);
    gtk_box_pack_start(GTK_BOX(cell), display->action_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(idle), strip, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), idle, FALSE, FALSE, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    label = gtk_label_new("Часовой пояс");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    display->timezone_combo = chooser_new(display, "Часовой пояс",
                                         timezone_changed);
    timezone = system_timezone(&timezone_error);
    if (!timezone_error)
        populate_timezones(display, timezone);
    else
        g_clear_error(&timezone_error);
    gtk_box_pack_start(GTK_BOX(row), display->timezone_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(time), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), time, FALSE, FALSE, 0);
    g_free(timezone);

    strip = make_strip();
    cell = strip_cell(strip, "Шрифт");
    display->font_button = gtk_font_button_new_with_font(display->config.clock_font);
    gtk_font_button_set_use_size(GTK_FONT_BUTTON(display->font_button), FALSE);
    /* The clock has its own size control, so the point size the chooser keeps
     * is meaningless here and only shows up as a stray number on the button. */
    gtk_font_button_set_show_size(GTK_FONT_BUTTON(display->font_button), FALSE);
    gtk_box_pack_start(GTK_BOX(cell), display->font_button, TRUE, TRUE, 0);
    cell = strip_cell(strip, "Язык даты");
    display->language_combo = chooser_new(display, "Язык даты",
                                         clock_settings_changed);
    chooser_add(display->language_combo, "ru", "Русский");
    chooser_add(display->language_combo, "en", "English");
    chooser_set_id(display->language_combo, display->config.clock_language);
    gtk_box_pack_start(GTK_BOX(cell), display->language_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(clock), strip, FALSE, FALSE, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    label = gtk_label_new("Размер цифр");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    display->clock_size_scale = make_scale(80, 360, display->config.clock_size);
    gtk_box_pack_start(GTK_BOX(row), display->clock_size_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(clock), row, FALSE, FALSE, 0);

    /* Night mode is one line: the switch, the level the clock turns red below,
     * and the level it goes back to white above. */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    display->night_check = gtk_check_button_new_with_label("Ночной режим");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(display->night_check),
                                 display->config.night_mode);
    gtk_box_pack_start(GTK_BOX(row), display->night_check, FALSE, FALSE, 0);
    cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    label = gtk_label_new("Красный ниже, lx");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(cell), label, FALSE, FALSE, 0);
    display->threshold_scale = make_scale(0, 500,
                                          display->config.night_threshold_lux);
    gtk_box_pack_start(GTK_BOX(cell), display->threshold_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), cell, TRUE, TRUE, 0);
    cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    label = gtk_label_new("Обратно выше, lx");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(cell), label, FALSE, FALSE, 0);
    display->release_scale = make_scale(0, 600,
                                        display->config.night_release_lux);
    gtk_box_pack_start(GTK_BOX(cell), display->release_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), cell, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(clock), row, FALSE, FALSE, 0);

    button = gtk_button_new_with_label("Показать часы");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    g_signal_connect(button, "clicked", G_CALLBACK(preview_clock), display);
    gtk_box_pack_start(GTK_BOX(clock), button, FALSE, FALSE, 0);
    g_signal_connect(display->font_button, "font-set",
                     G_CALLBACK(clock_settings_changed), display);
    g_signal_connect(display->clock_size_scale, "value-changed",
                     G_CALLBACK(clock_settings_changed), display);
    g_signal_connect(display->night_check, "toggled",
                     G_CALLBACK(clock_settings_changed), display);
    g_signal_connect(display->threshold_scale, "value-changed",
                     G_CALLBACK(clock_settings_changed), display);
    g_signal_connect(display->release_scale, "value-changed",
                     G_CALLBACK(clock_settings_changed), display);
    gtk_box_pack_start(GTK_BOX(content), clock, FALSE, FALSE, 0);
    return page;
}

static void build_settings(struct chiron_display *display)
{
    GtkWidget *header;
    GtkWidget *notebook;
    GtkWidget *frame;
    GtkWidget *footer;
    GtkCssProvider *provider;

    display->building_ui = TRUE;
    display->settings = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(display->settings),
                         "Экран и освещённость");
    gtk_window_set_default_size(GTK_WINDOW(display->settings), 1050, 850);
    gtk_window_maximize(GTK_WINDOW(display->settings));
    gtk_style_context_add_class(gtk_widget_get_style_context(display->settings),
                                "chiron-settings");
    g_signal_connect(display->settings, "delete-event",
                     G_CALLBACK(settings_delete), display);
    header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Экран и освещённость");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(display->settings), header);
    notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             build_brightness_page(display),
                             gtk_label_new("  Яркость  "));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             build_screen_page(display),
                             gtk_label_new("  Экран и часы  "));
    gtk_widget_set_vexpand(notebook, TRUE);
    frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(frame), notebook, TRUE, TRUE, 0);
    footer = gtk_label_new("angellfear");
    gtk_label_set_xalign(GTK_LABEL(footer), 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(footer), "footer");
    gtk_box_pack_start(GTK_BOX(frame), footer, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(display->settings), frame);
    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, settings_css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    display->building_ui = FALSE;
    refresh_reading_safe(display);
}

static void show_settings(struct chiron_display *display)
{
    if (!display->settings)
        build_settings(display);
    gtk_widget_show_all(display->settings);
    gtk_window_present(GTK_WINDOW(display->settings));
}

int chiron_display_remote_command(int argc, char **argv)
{
    const char *method = NULL;
    GVariant *parameters = NULL;
    GDBusConnection *bus;
    GError *error = NULL;
    GVariant *reply;

    if (argc == 2 && strcmp(argv[1], "--settings") == 0)
        method = "ShowSettings";
    else if (argc == 3 && strcmp(argv[1], "--clock") == 0 &&
             strcmp(argv[2], "show") == 0)
        method = "ShowClock";
    else if (argc == 3 && strcmp(argv[1], "--clock") == 0 &&
             strcmp(argv[2], "hide") == 0)
        method = "HideClock";
    else if (argc == 3 && strcmp(argv[1], "--screen-off") == 0 &&
             (strcmp(argv[2], "0") == 0 || strcmp(argv[2], "1") == 0)) {
        method = "SetScreenOff";
        parameters = g_variant_new("(b)", strcmp(argv[2], "1") == 0);
    } else {
        return -1;
    }
    if (!parameters)
        parameters = g_variant_new("()");
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus) {
        g_printerr("chiron-privacy: no session bus: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    reply = g_dbus_connection_call_sync(bus, DISPLAY_NAME, DISPLAY_PATH,
                                        DISPLAY_IFACE, method, parameters, NULL,
                                        G_DBUS_CALL_FLAGS_NONE, 4000, NULL,
                                        &error);
    g_object_unref(bus);
    if (!reply) {
        g_printerr("chiron-privacy: display controller unavailable: %s\n",
                   error->message);
        g_error_free(error);
        return 1;
    }
    g_variant_unref(reply);
    return 0;
}

struct chiron_display *chiron_display_new(GDBusConnection *bus, int lux,
                                           gboolean lux_known)
{
    struct chiron_display *display = g_new0(struct chiron_display, 1);
    GDBusNodeInfo *sni_info;
    GDBusNodeInfo *control_info;
    GError *error = NULL;

    display->bus = g_object_ref(bus);
    display->lux = lux;
    display->lux_known = lux_known;
    /* The marker survives a controller restart within the same login session,
     * so a restarted ALS watcher cannot light the backlight behind the blanker. */
    display->screen_off = screen_off_marker_exists();
    display->applied_percent = read_backlight_percent();
    display->selected_point = -1;
    display->dragged_point = -1;
    load_config(&display->config);
    save_config(display);
    /* If idle started first, HUP makes it reread the same persisted choice;
     * if it has not started yet, the freshly written file is what it opens. */
    save_power(display, TRUE);
    build_clock(display);
    build_blanker(display);
    if (display->screen_off)
        gtk_widget_show_all(display->blanker);
    sni_info = g_dbus_node_info_new_for_xml(sni_xml, &error);
    if (!sni_info)
        g_error("invalid SNI interface: %s", error->message);
    display->sni_object_id = g_dbus_connection_register_object(
        bus, SNI_PATH, sni_info->interfaces[0], &sni_vtable, display, NULL,
        &error);
    if (!display->sni_object_id)
        g_error("cannot export SNI: %s", error->message);
    g_dbus_node_info_unref(sni_info);
    control_info = g_dbus_node_info_new_for_xml(control_xml, &error);
    if (!control_info)
        g_error("invalid display interface: %s", error->message);
    display->control_object_id = g_dbus_connection_register_object(
        bus, DISPLAY_PATH, control_info->interfaces[0], &control_vtable,
        display, NULL, &error);
    if (!display->control_object_id)
        g_error("cannot export display controller: %s", error->message);
    g_dbus_node_info_unref(control_info);
    display->display_name_id = g_bus_own_name_on_connection(
        bus, DISPLAY_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, display,
        NULL);
    display->sni_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1",
                                        getpid());
    display->sni_name_id = g_bus_own_name_on_connection(
        bus, display->sni_name, G_BUS_NAME_OWNER_FLAGS_NONE, sni_name_acquired,
        NULL, display, NULL);
    display->watcher_id = g_bus_watch_name_on_connection(
        bus, SNI_WATCHER, G_BUS_NAME_WATCHER_FLAGS_NONE, watcher_appeared,
        watcher_vanished, display, NULL);
    if (display->config.use_sensor && display->lux_known)
        request_brightness(display, target_brightness(display), FALSE);
    return display;
}

/* Push one reading into the ten-second window and return the mean of what is
 * still inside it. Readings arrive irregularly, so the window is held by
 * timestamp rather than by count. */
static int lux_average(struct chiron_display *display, int lux)
{
    gint64 now = g_get_monotonic_time();
    long long total = 0;
    int used = 0;
    int i;

    display->lux_window[display->lux_next].when_us = now;
    display->lux_window[display->lux_next].lux = lux;
    display->lux_next = (display->lux_next + 1) % LUX_SAMPLES;
    if (display->lux_count < LUX_SAMPLES)
        display->lux_count++;
    for (i = 0; i < display->lux_count; i++) {
        if (now - display->lux_window[i].when_us > LUX_WINDOW_US)
            continue;
        total += display->lux_window[i].lux;
        used++;
    }
    return used ? (int)(total / used) : lux;
}

void chiron_display_set_lux(struct chiron_display *display, int lux,
                            gboolean known)
{
    if (known) {
        display->lux = lux_average(display, MAX(0, lux));
    } else {
        /* The sensor went away; keep nothing to average. */
        display->lux_count = 0;
        display->lux_next = 0;
        display->lux = MAX(0, lux);
    }
    display->lux_known = known;
    update_tray(display);
    refresh_reading_safe(display);
    refresh_clock_style(display);
    if (known && display->config.use_sensor)
        request_brightness(display, target_brightness(display), FALSE);
}

/* The two hardware switches share the one tray item this process owns. Their
 * positions live here so the icon, the tooltip and the attention state are all
 * answered from the same place the panel asks. */
void chiron_display_set_privacy(struct chiron_display *display,
                                gboolean camera_closed, gboolean mic_muted)
{
    if (display->camera_closed == camera_closed &&
        display->mic_muted == mic_muted)
        return;
    display->camera_closed = camera_closed;
    display->mic_muted = mic_muted;
    update_tray(display);
}
