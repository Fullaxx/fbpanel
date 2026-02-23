/*
 * loadavg.c -- fbpanel system load average plugin.
 *
 * Displays the 1-, 5-, and/or 15-minute load averages from /proc/loadavg
 * as a text label on the panel.
 *
 * Soft-disable behaviour:
 *   If /proc/loadavg cannot be opened at startup (e.g. procfs not mounted,
 *   running inside a container without procfs), the constructor emits a
 *   g_message() and returns 0.  The panel skips the plugin and continues
 *   loading normally.
 *
 * Configuration (xconf keys):
 *   Show1    — boolean; show 1-minute average  (default: true).
 *   Show5    — boolean; show 5-minute average  (default: true).
 *   Show15   — boolean; show 15-minute average (default: false).
 *   Period   — update interval in milliseconds  (default: 5000).
 *
 * Data source:
 *   /proc/loadavg — first three whitespace-separated fields are the 1-, 5-,
 *   and 15-minute exponential moving averages of the run-queue length.
 *
 * Widget hierarchy:
 *   p->pwid (GtkBgbox, managed by framework)
 *     priv->label (GtkLabel)
 */

#include <stdio.h>
#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * loadavg_priv -- per-instance private state.
 *
 * plugin  -- base class (MUST be first; cast to plugin_instance*).
 * label   -- GtkLabel showing the formatted load string.
 * timer   -- GLib timeout source ID; 0 when inactive.
 * show1   -- display the 1-min average when non-zero.
 * show5   -- display the 5-min average when non-zero.
 * show15  -- display the 15-min average when non-zero.
 * period  -- polling interval in milliseconds.
 */
typedef struct {
    plugin_instance plugin;
    GtkWidget      *label;
    guint           timer;
    int             show1;
    int             show5;
    int             show15;
    int             period;
} loadavg_priv;

/*
 * loadavg_update -- read /proc/loadavg and refresh the label.
 *
 * Formats the selected averages as a space-separated string and sets
 * the label text plus a full tooltip showing all three values.
 * If /proc/loadavg cannot be opened, the label shows "n/a".
 *
 * Parameters:
 *   priv -- loadavg_priv instance.
 *
 * Returns: TRUE to keep the GLib timer repeating.
 */
static gboolean
loadavg_update(loadavg_priv *priv)
{
    FILE  *f;
    float  avg1, avg5, avg15;
    gchar  display[64];
    gchar  tooltip[96];
    int    pos = 0;

    ENTER;

    f = fopen("/proc/loadavg", "r");
    if (!f) {
        gtk_label_set_text(GTK_LABEL(priv->label), "n/a");
        RET(TRUE);
    }

    if (fscanf(f, "%f %f %f", &avg1, &avg5, &avg15) != 3) {
        fclose(f);
        gtk_label_set_text(GTK_LABEL(priv->label), "n/a");
        RET(TRUE);
    }
    fclose(f);

    display[0] = '\0';
    if (priv->show1) {
        pos += g_snprintf(display + pos, sizeof(display) - pos,
                          "%.2f", avg1);
    }
    if (priv->show5) {
        if (pos > 0 && pos < (int)sizeof(display) - 1)
            display[pos++] = ' ';
        pos += g_snprintf(display + pos, sizeof(display) - pos,
                          "%.2f", avg5);
    }
    if (priv->show15) {
        if (pos > 0 && pos < (int)sizeof(display) - 1)
            display[pos++] = ' ';
        g_snprintf(display + pos, sizeof(display) - pos,
                   "%.2f", avg15);
    }
    if (display[0] == '\0')
        g_strlcpy(display, "—", sizeof(display));

    gtk_label_set_text(GTK_LABEL(priv->label), display);

    g_snprintf(tooltip, sizeof(tooltip),
               "<b>Load average:</b>\n"
               "1 min:  %.2f\n5 min:  %.2f\n15 min: %.2f",
               avg1, avg5, avg15);
    gtk_widget_set_tooltip_markup(priv->plugin.pwid, tooltip);

    DBG("loadavg: %s\n", display);
    RET(TRUE);
}

/*
 * loadavg_constructor -- initialise the load average plugin.
 *
 * Probes /proc/loadavg; returns 0 (soft-disable) if it is unreadable.
 * Creates a GtkLabel, reads config, and starts the polling timer.
 *
 * Parameters:
 *   p -- plugin_instance allocated by the framework.
 *
 * Returns: 1 on success, 0 on failure.
 */
static int
loadavg_constructor(plugin_instance *p)
{
    loadavg_priv *priv;
    FILE         *probe;

    ENTER;

    /* Probe at startup: if /proc/loadavg is unreadable, disable cleanly. */
    probe = fopen("/proc/loadavg", "r");
    if (!probe) {
        g_message("loadavg: /proc/loadavg not available — plugin disabled");
        RET(0);
    }
    fclose(probe);

    priv = (loadavg_priv *) p;

    /* Defaults */
    priv->show1  = 1;
    priv->show5  = 1;
    priv->show15 = 0;
    priv->period = 5000;

    XCG(p->xc, "Show1",  &priv->show1,  enum, bool_enum);
    XCG(p->xc, "Show5",  &priv->show5,  enum, bool_enum);
    XCG(p->xc, "Show15", &priv->show15, enum, bool_enum);
    XCG(p->xc, "Period", &priv->period, int);

    if (priv->period < 500)
        priv->period = 500;

    priv->label = gtk_label_new("...");
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);

    loadavg_update(priv);
    priv->timer = g_timeout_add(priv->period,
                                (GSourceFunc) loadavg_update, priv);
    RET(1);
}

/*
 * loadavg_destructor -- clean up load average plugin resources.
 *
 * Removes the polling timer.  GTK widgets are owned by p->pwid and
 * destroyed automatically by the framework.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 */
static void
loadavg_destructor(plugin_instance *p)
{
    loadavg_priv *priv = (loadavg_priv *) p;

    ENTER;
    if (priv->timer) {
        g_source_remove(priv->timer);
        priv->timer = 0;
    }
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "loadavg",
    .name        = "Load Average",
    .version     = "1.0",
    .description = "Display system load averages from /proc/loadavg",
    .priv_size   = sizeof(loadavg_priv),
    .constructor = loadavg_constructor,
    .destructor  = loadavg_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
