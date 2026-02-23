/*
 * brightness.c -- fbpanel screen brightness plugin.
 *
 * Displays the current backlight brightness level as a text percentage
 * and allows adjustment via the scroll wheel.
 *
 * Soft-disable behaviour:
 *   If no backlight device is found under /sys/class/backlight/ (e.g.
 *   running on a desktop without a backlight driver, or in a container),
 *   the constructor emits g_message() and returns 0.  The panel skips
 *   the plugin and continues loading normally.
 *
 * Write permission:
 *   Adjusting brightness requires write access to the brightness sysfs
 *   node (typically /sys/class/backlight/<dev>/brightness).  On most
 *   systems this requires either membership in the 'video' group (via a
 *   udev rule such as "TAG+=\"uaccess\"") or running fbpanel as root.
 *   Read-only display works for any user.
 *
 * Configuration (xconf keys):
 *   Device -- backlight device name (default: auto-detected, first entry
 *             found under /sys/class/backlight/).
 *   Step   -- brightness adjustment step in percent of max (default: 5).
 *   Period -- update interval in milliseconds (default: 2000).
 *
 * Data source:
 *   /sys/class/backlight/<dev>/brightness     -- current raw brightness.
 *   /sys/class/backlight/<dev>/max_brightness -- maximum raw brightness.
 *   Percentage = (brightness / max_brightness) * 100.
 *
 * Widget hierarchy:
 *   p->pwid (GtkBgbox, managed by framework)
 *     priv->label (GtkLabel showing "XX%")
 *       connected to: "scroll-event" for brightness adjustment
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/* sysfs directory containing one subdirectory per backlight device. */
#define BACKLIGHT_DIR "/sys/class/backlight"

/*
 * brightness_priv -- per-instance private state.
 *
 * plugin       -- base class (MUST be first).
 * label        -- GtkLabel showing the brightness percentage.
 * timer        -- GLib timeout source ID; 0 when inactive.
 * cfg_device   -- device name from config (non-owning xconf ptr or NULL).
 * bright_path  -- heap path to the brightness file; g_free in destructor.
 * max_path     -- heap path to the max_brightness file; g_free in destructor.
 * max_value    -- cached maximum brightness (read once at startup).
 * step_pct     -- scroll-wheel step as percent of max (1-50).
 * period       -- polling interval in milliseconds.
 */
typedef struct {
    plugin_instance  plugin;
    GtkWidget       *label;
    guint            timer;
    gchar           *cfg_device;
    gchar           *bright_path;
    gchar           *max_path;
    int              max_value;
    int              step_pct;
    int              period;
} brightness_priv;

/*
 * brightness_read -- read an integer value from a sysfs file.
 *
 * Parameters:
 *   path -- full path to sysfs file.
 *   val  -- output: integer value read from the file.
 *
 * Returns: 0 on success, -1 on failure.
 */
static int
brightness_read(const gchar *path, int *val)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%d", val) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/*
 * brightness_write -- write an integer value to a sysfs file.
 *
 * Parameters:
 *   path -- full path to sysfs brightness file.
 *   val  -- value to write.
 *
 * Returns: 0 on success, -1 on failure (e.g. permission denied).
 */
static int
brightness_write(const gchar *path, int val)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "%d\n", val);
    fclose(f);
    return 0;
}

/*
 * brightness_update -- read current brightness and refresh the label.
 *
 * Calculates the percentage and updates the label text and tooltip.
 * If the sysfs read fails (device removed at runtime), shows "n/a".
 *
 * Parameters:
 *   priv -- brightness_priv instance.
 *
 * Returns: TRUE to keep the GLib timer repeating.
 */
static gboolean
brightness_update(brightness_priv *priv)
{
    int   cur = 0;
    int   pct;
    gchar label_text[16];
    gchar tooltip[80];

    ENTER;

    if (brightness_read(priv->bright_path, &cur) != 0) {
        gtk_label_set_text(GTK_LABEL(priv->label), "n/a");
        RET(TRUE);
    }

    pct = (priv->max_value > 0)
          ? (int)((double) cur / (double) priv->max_value * 100.0 + 0.5)
          : 0;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    g_snprintf(label_text, sizeof(label_text), "%d%%", pct);
    gtk_label_set_text(GTK_LABEL(priv->label), label_text);

    g_snprintf(tooltip, sizeof(tooltip),
               "<b>Brightness:</b> %d%%\n"
               "Raw: %d / %d\n"
               "Scroll to adjust (step: %d%%)",
               pct, cur, priv->max_value, priv->step_pct);
    gtk_widget_set_tooltip_markup(priv->plugin.pwid, tooltip);

    DBG("brightness: cur=%d max=%d pct=%d\n", cur, priv->max_value, pct);
    RET(TRUE);
}

/*
 * brightness_scrolled -- "scroll-event" handler for brightness adjustment.
 *
 * Scroll up/right increases brightness by step_pct percent of max.
 * Scroll down/left decreases brightness.  Values are clamped to [0, max].
 * If the write fails (permission denied), the adjustment is silently
 * ignored; the label will still show the current readable value.
 *
 * Parameters:
 *   widget -- the label widget receiving the scroll event.
 *   event  -- GdkEventScroll.
 *   priv   -- brightness_priv instance.
 *
 * Returns: TRUE (event consumed).
 */
static gboolean
brightness_scrolled(GtkWidget *widget, GdkEventScroll *event,
                    brightness_priv *priv)
{
    int cur = 0;
    int step;
    int newval;

    ENTER;

    if (brightness_read(priv->bright_path, &cur) != 0)
        RET(TRUE);

    step = (priv->max_value * priv->step_pct) / 100;
    if (step < 1)
        step = 1;

    if (event->direction == GDK_SCROLL_UP ||
        event->direction == GDK_SCROLL_RIGHT) {
        newval = cur + step;
    } else {
        newval = cur - step;
    }

    if (newval < 0)              newval = 0;
    if (newval > priv->max_value) newval = priv->max_value;

    brightness_write(priv->bright_path, newval);
    brightness_update(priv);

    RET(TRUE);
}

/*
 * brightness_find_device -- locate the first backlight device in sysfs.
 *
 * Scans BACKLIGHT_DIR and returns a newly-allocated string with the
 * first non-hidden entry name, or NULL if none are found.
 *
 * Caller must g_free() the returned string.
 */
static gchar *
brightness_find_device(void)
{
    DIR           *dir;
    struct dirent *ent;
    gchar         *result = NULL;

    dir = opendir(BACKLIGHT_DIR);
    if (!dir)
        return NULL;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        result = g_strdup(ent->d_name);
        break;
    }
    closedir(dir);
    return result;
}

/*
 * brightness_constructor -- initialise the backlight brightness plugin.
 *
 * Auto-detects the backlight device if Device is not configured.
 * Probes the brightness sysfs file and reads max_brightness.
 * Returns 0 (soft-disable) if no backlight device is found.
 *
 * Returns: 1 on success, 0 on soft-disable.
 */
static int
brightness_constructor(plugin_instance *p)
{
    brightness_priv *priv;
    gchar           *device = NULL;
    int              max    = 0;
    gchar           *bright_path = NULL;
    gchar           *max_path    = NULL;

    ENTER;

    priv = (brightness_priv *) p;
    priv->cfg_device = NULL;
    priv->step_pct   = 5;
    priv->period     = 2000;

    XCG(p->xc, "Device", &priv->cfg_device, str);
    XCG(p->xc, "Step",   &priv->step_pct,   int);
    XCG(p->xc, "Period", &priv->period,      int);

    if (priv->step_pct < 1)   priv->step_pct = 1;
    if (priv->step_pct > 50)  priv->step_pct = 50;
    if (priv->period   < 500) priv->period    = 500;

    /* Use configured device if given, otherwise auto-detect. */
    if (priv->cfg_device && priv->cfg_device[0] != '\0') {
        device = g_strdup(priv->cfg_device);
    } else {
        device = brightness_find_device();
    }

    if (!device) {
        g_message("brightness: no backlight device found in "
                  BACKLIGHT_DIR " — plugin disabled");
        RET(0);
    }

    bright_path = g_strdup_printf(BACKLIGHT_DIR "/%s/brightness",     device);
    max_path    = g_strdup_printf(BACKLIGHT_DIR "/%s/max_brightness",  device);
    g_free(device);

    /* Probe brightness file. */
    {
        FILE *probe = fopen(bright_path, "r");
        if (!probe) {
            g_message("brightness: %s not readable — plugin disabled",
                      bright_path);
            g_free(bright_path);
            g_free(max_path);
            RET(0);
        }
        fclose(probe);
    }

    /* Read max_brightness once at startup. */
    if (brightness_read(max_path, &max) != 0 || max <= 0) {
        g_message("brightness: could not read max_brightness from %s"
                  " — plugin disabled", max_path);
        g_free(bright_path);
        g_free(max_path);
        RET(0);
    }

    priv->bright_path = bright_path;
    priv->max_path    = max_path;
    priv->max_value   = max;

    priv->label = gtk_label_new("...");
    gtk_widget_add_events(priv->label, GDK_SCROLL_MASK);
    g_signal_connect(G_OBJECT(priv->label), "scroll-event",
                     G_CALLBACK(brightness_scrolled), priv);
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);

    brightness_update(priv);
    priv->timer = g_timeout_add(priv->period,
                                (GSourceFunc) brightness_update, priv);
    RET(1);
}

/*
 * brightness_destructor -- clean up brightness plugin resources.
 *
 * Cancels the timer and frees heap-allocated sysfs paths.
 * The scroll-event signal on priv->label is disconnected automatically
 * when the label widget is destroyed by the framework (p->pwid destruction).
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 */
static void
brightness_destructor(plugin_instance *p)
{
    brightness_priv *priv = (brightness_priv *) p;

    ENTER;
    if (priv->timer) {
        g_source_remove(priv->timer);
        priv->timer = 0;
    }
    g_free(priv->bright_path);
    priv->bright_path = NULL;
    g_free(priv->max_path);
    priv->max_path = NULL;
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "brightness",
    .name        = "Brightness",
    .version     = "1.0",
    .description = "Display and control screen backlight brightness",
    .priv_size   = sizeof(brightness_priv),
    .constructor = brightness_constructor,
    .destructor  = brightness_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
