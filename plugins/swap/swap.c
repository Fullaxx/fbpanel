/*
 * swap.c -- fbpanel swap usage plugin.
 *
 * Displays swap space utilisation as a GtkProgressBar, styled to match the
 * existing mem plugin.  Data is read from /proc/meminfo every 3 seconds.
 *
 * Soft-disable behaviour:
 *   If /proc/meminfo cannot be opened at startup the constructor emits
 *   g_message() and returns 0.  If the system has no swap (SwapTotal == 0)
 *   and HideIfNoSwap is true (the default), the constructor also returns 0
 *   so the plugin does not appear on the panel.
 *
 * Configuration (xconf keys):
 *   HideIfNoSwap — boolean; disable plugin when no swap is configured
 *                  (default: true).
 *   Period       — update interval in milliseconds (default: 3000).
 *
 * Data source:
 *   /proc/meminfo — SwapTotal and SwapFree fields (values in kB).
 *   swap_used = SwapTotal - SwapFree.
 *
 * Widget hierarchy:
 *   p->pwid (GtkBgbox, managed by framework)
 *     priv->pb (GtkProgressBar, oriented per panel orientation)
 */

#include <stdio.h>
#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * swap_priv -- per-instance private state.
 *
 * plugin         -- base class (MUST be first).
 * pb             -- GtkProgressBar for swap usage.
 * timer          -- GLib timeout source ID; 0 when inactive.
 * hide_if_no_swap -- non-zero: disable cleanly when SwapTotal == 0.
 * period         -- polling interval in milliseconds.
 */
typedef struct {
    plugin_instance plugin;
    GtkWidget      *pb;
    guint           timer;
    int             hide_if_no_swap;
    int             period;
} swap_priv;

/*
 * swap_read -- parse SwapTotal and SwapFree from /proc/meminfo.
 *
 * Parameters:
 *   total_kb -- output: SwapTotal in kB.
 *   free_kb  -- output: SwapFree  in kB.
 *
 * Returns: 0 on success, -1 if /proc/meminfo is unreadable or fields
 *          are not found.
 */
static int
swap_read(gulong *total_kb, gulong *free_kb)
{
    FILE *f;
    char  buf[128];
    int   got_total = 0, got_free = 0;

    *total_kb = *free_kb = 0;

    f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;

    while (fgets(buf, sizeof(buf), f)) {
        if (!got_total && strncmp(buf, "SwapTotal:", 10) == 0) {
            sscanf(buf + 10, "%lu", total_kb);
            got_total = 1;
        } else if (!got_free && strncmp(buf, "SwapFree:", 9) == 0) {
            sscanf(buf + 9, "%lu", free_kb);
            got_free = 1;
        }
        if (got_total && got_free)
            break;
    }
    fclose(f);
    return (got_total && got_free) ? 0 : -1;
}

/*
 * swap_update -- refresh the progress bar and tooltip.
 *
 * Reads current swap counters and updates the GtkProgressBar fraction
 * and tooltip text.  When SwapTotal == 0, the bar is set to 0 and the
 * tooltip reports "No swap configured".
 *
 * Parameters:
 *   priv -- swap_priv instance.
 *
 * Returns: TRUE to keep the GLib timer repeating.
 */
static gboolean
swap_update(swap_priv *priv)
{
    gulong total_kb, free_kb, used_kb;
    gdouble fraction;
    gchar tooltip[96];

    ENTER;

    if (swap_read(&total_kb, &free_kb) != 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(priv->pb), 0.0);
        gtk_widget_set_tooltip_markup(priv->plugin.pwid,
                                      "<b>Swap:</b> unavailable");
        RET(TRUE);
    }

    if (total_kb == 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(priv->pb), 0.0);
        gtk_widget_set_tooltip_markup(priv->plugin.pwid,
                                      "<b>Swap:</b> no swap configured");
        RET(TRUE);
    }

    used_kb  = total_kb - free_kb;
    fraction = (gdouble) used_kb / (gdouble) total_kb;

    DBG("swap: used=%lu total=%lu frac=%.2f\n", used_kb, total_kb, fraction);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(priv->pb), fraction);

    g_snprintf(tooltip, sizeof(tooltip),
               "<b>Swap:</b> %d%%, %lu MB of %lu MB",
               (int)(fraction * 100),
               used_kb  >> 10,
               total_kb >> 10);
    gtk_widget_set_tooltip_markup(priv->plugin.pwid, tooltip);

    RET(TRUE);
}

/*
 * swap_constructor -- initialise the swap plugin.
 *
 * Probes /proc/meminfo; soft-disables if unreadable or if HideIfNoSwap is
 * set and no swap is configured.  Creates the GtkProgressBar and timer.
 *
 * Returns: 1 on success, 0 on soft-disable.
 */
static int
swap_constructor(plugin_instance *p)
{
    swap_priv *priv;
    gulong     total_kb, free_kb;
    gint       w, h;
    GtkProgressBarOrientation orient;

    ENTER;

    priv = (swap_priv *) p;
    priv->hide_if_no_swap = 1;
    priv->period          = 3000;

    XCG(p->xc, "HideIfNoSwap", &priv->hide_if_no_swap, enum, bool_enum);
    XCG(p->xc, "Period",       &priv->period,           int);

    if (priv->period < 500)
        priv->period = 500;

    if (swap_read(&total_kb, &free_kb) != 0) {
        g_message("swap: /proc/meminfo not available — plugin disabled");
        RET(0);
    }

    if (priv->hide_if_no_swap && total_kb == 0) {
        g_message("swap: no swap configured — plugin disabled");
        RET(0);
    }

    /* Match mem.c orientation logic: vertical bar on a horizontal panel. */
    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        orient = GTK_PROGRESS_BOTTOM_TO_TOP;
        w = 9;
        h = 0;
    } else {
        orient = GTK_PROGRESS_LEFT_TO_RIGHT;
        w = 0;
        h = 9;
    }

    priv->pb = gtk_progress_bar_new();
    gtk_progress_bar_set_orientation(GTK_PROGRESS_BAR(priv->pb), orient);
    gtk_widget_set_size_request(priv->pb, w, h);
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->pb);
    gtk_widget_show(priv->pb);

    swap_update(priv);
    priv->timer = g_timeout_add(priv->period,
                                (GSourceFunc) swap_update, priv);
    RET(1);
}

/*
 * swap_destructor -- clean up swap plugin resources.
 *
 * Cancels the polling timer.  GTK widgets are destroyed by the framework.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 */
static void
swap_destructor(plugin_instance *p)
{
    swap_priv *priv = (swap_priv *) p;

    ENTER;
    if (priv->timer) {
        g_source_remove(priv->timer);
        priv->timer = 0;
    }
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "swap",
    .name        = "Swap Usage",
    .version     = "1.0",
    .description = "Display swap usage from /proc/meminfo",
    .priv_size   = sizeof(swap_priv),
    .constructor = swap_constructor,
    .destructor  = swap_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
