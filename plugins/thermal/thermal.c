/*
 * thermal.c -- fbpanel CPU/board temperature plugin.
 *
 * Reads a thermal zone temperature from the kernel's thermal sysfs
 * interface and displays it as a text label (e.g. "45°C").
 *
 * Soft-disable behaviour:
 *   If the requested thermal zone does not exist (e.g. running inside a
 *   container, on hardware without thermal sensors, or when the relevant
 *   kernel module is not loaded), the constructor emits g_message() and
 *   returns 0.  The panel skips the plugin and continues loading.
 *
 * Configuration (xconf keys):
 *   ThermalZone -- integer zone index N (default: 0).
 *                  Reads /sys/class/thermal/thermal_zone<N>/temp.
 *   WarnTemp    -- temperature in °C at which the label turns orange
 *                  (default: 70).
 *   CritTemp    -- temperature in °C at which the label turns red
 *                  (default: 90).
 *   Period      -- update interval in milliseconds (default: 5000).
 *
 * Data source:
 *   /sys/class/thermal/thermal_zone<N>/temp
 *   Value is in millidegrees Celsius.  Divided by 1000 to get °C.
 *
 * Colour coding:
 *   < WarnTemp  -- no markup (default theme foreground)
 *   >= WarnTemp -- orange
 *   >= CritTemp -- red
 */

#include <stdio.h>
#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * thermal_priv -- per-instance private state.
 *
 * plugin    -- base class (MUST be first).
 * label     -- GtkLabel showing the temperature string.
 * timer     -- GLib timeout source ID; 0 when inactive.
 * zone      -- thermal zone index.
 * warn_temp -- °C threshold for orange colour.
 * crit_temp -- °C threshold for red colour.
 * period    -- polling interval in milliseconds.
 * path      -- heap-allocated sysfs path; g_free'd in destructor.
 */
typedef struct {
    plugin_instance  plugin;
    GtkWidget       *label;
    guint            timer;
    int              zone;
    int              warn_temp;
    int              crit_temp;
    int              period;
    gchar           *path;
} thermal_priv;

/*
 * thermal_update -- read current temperature and refresh the label.
 *
 * Applies colour markup based on WarnTemp and CritTemp thresholds.
 * If the sysfs file is unreadable (e.g. hardware removed at runtime),
 * the label shows "n/a" without colour.
 *
 * Parameters:
 *   priv -- thermal_priv instance.
 *
 * Returns: TRUE to keep the GLib timer repeating.
 */
static gboolean
thermal_update(thermal_priv *priv)
{
    FILE  *f;
    long   millideg = 0;
    int    deg;
    gchar  markup[64];
    gchar  tooltip[80];

    ENTER;

    f = fopen(priv->path, "r");
    if (!f) {
        gtk_label_set_markup(GTK_LABEL(priv->label), "n/a");
        RET(TRUE);
    }
    (void) fscanf(f, "%ld", &millideg);
    fclose(f);

    deg = (int)(millideg / 1000);

    DBG("thermal: zone%d = %d°C\n", priv->zone, deg);

    if (deg >= priv->crit_temp) {
        g_snprintf(markup, sizeof(markup),
                   "<span foreground='red'>%d°C</span>", deg);
    } else if (deg >= priv->warn_temp) {
        g_snprintf(markup, sizeof(markup),
                   "<span foreground='orange'>%d°C</span>", deg);
    } else {
        g_snprintf(markup, sizeof(markup), "%d°C", deg);
    }

    gtk_label_set_markup(GTK_LABEL(priv->label), markup);

    g_snprintf(tooltip, sizeof(tooltip),
               "<b>Thermal zone %d:</b> %d°C\n"
               "Warn: %d°C  Critical: %d°C",
               priv->zone, deg, priv->warn_temp, priv->crit_temp);
    gtk_widget_set_tooltip_markup(priv->plugin.pwid, tooltip);

    RET(TRUE);
}

/*
 * thermal_constructor -- initialise the thermal plugin.
 *
 * Reads config, builds the sysfs path, and probes it.  Returns 0
 * (soft-disable) if the path does not exist.
 *
 * Returns: 1 on success, 0 on soft-disable.
 */
static int
thermal_constructor(plugin_instance *p)
{
    thermal_priv *priv;
    FILE         *probe;

    ENTER;

    priv = (thermal_priv *) p;
    priv->zone      = 0;
    priv->warn_temp = 70;
    priv->crit_temp = 90;
    priv->period    = 5000;

    XCG(p->xc, "ThermalZone", &priv->zone,      int);
    XCG(p->xc, "WarnTemp",    &priv->warn_temp,  int);
    XCG(p->xc, "CritTemp",    &priv->crit_temp,  int);
    XCG(p->xc, "Period",      &priv->period,      int);

    if (priv->period < 500)
        priv->period = 500;
    if (priv->zone < 0)
        priv->zone = 0;

    priv->path = g_strdup_printf(
        "/sys/class/thermal/thermal_zone%d/temp", priv->zone);

    probe = fopen(priv->path, "r");
    if (!probe) {
        g_message("thermal: %s not available — plugin disabled", priv->path);
        g_free(priv->path);
        priv->path = NULL;
        RET(0);
    }
    fclose(probe);

    priv->label = gtk_label_new("...");
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);

    thermal_update(priv);
    priv->timer = g_timeout_add(priv->period,
                                (GSourceFunc) thermal_update, priv);
    RET(1);
}

/*
 * thermal_destructor -- clean up thermal plugin resources.
 *
 * Cancels the timer and frees the heap-allocated sysfs path.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 */
static void
thermal_destructor(plugin_instance *p)
{
    thermal_priv *priv = (thermal_priv *) p;

    ENTER;
    if (priv->timer) {
        g_source_remove(priv->timer);
        priv->timer = 0;
    }
    g_free(priv->path);
    priv->path = NULL;
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "thermal",
    .name        = "Thermal Monitor",
    .version     = "1.0",
    .description = "Display CPU/board temperature from thermal sysfs",
    .priv_size   = sizeof(thermal_priv),
    .constructor = thermal_constructor,
    .destructor  = thermal_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
