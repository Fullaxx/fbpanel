/*
 * diskspace.c -- fbpanel disk space plugin.
 *
 * Displays used/free space for a configured filesystem mount point as a
 * GtkProgressBar.  The tooltip shows absolute usage in human-readable form.
 *
 * Soft-disable behaviour:
 *   If statvfs() fails for the configured path at startup (mount point does
 *   not exist or is not accessible), the constructor emits g_message() and
 *   returns 0.  The panel skips the plugin and continues loading normally.
 *
 * Configuration (xconf keys):
 *   MountPoint -- filesystem path to monitor (default: "/").
 *   Period     -- update interval in milliseconds (default: 10000).
 *
 * Data source:
 *   statvfs(3) — standard POSIX call; no external dependencies.
 *   used  = (f_blocks - f_bfree) * f_frsize
 *   total = f_blocks * f_frsize
 *   (Uses f_bfree rather than f_bavail so the bar reflects actual block
 *    usage including blocks reserved for root, matching df -h behaviour.)
 *
 * Widget hierarchy:
 *   p->pwid (GtkBgbox, managed by framework)
 *     priv->pb (GtkProgressBar, oriented per panel orientation)
 */

#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * diskspace_priv -- per-instance private state.
 *
 * plugin     -- base class (MUST be first).
 * pb         -- GtkProgressBar for disk usage.
 * timer      -- GLib timeout source ID; 0 when inactive.
 * mountpoint -- filesystem path to stat; non-owning pointer into xconf.
 * period     -- polling interval in milliseconds.
 */
typedef struct {
    plugin_instance  plugin;
    GtkWidget       *pb;
    guint            timer;
    gchar           *mountpoint;
    int              period;
} diskspace_priv;

/*
 * diskspace_update -- call statvfs and refresh the progress bar.
 *
 * If statvfs fails at runtime the bar shows full (1.0) and the tooltip
 * reports the error, allowing the user to notice without crashing.
 *
 * Parameters:
 *   priv -- diskspace_priv instance.
 *
 * Returns: TRUE to keep the GLib timer repeating.
 */
static gboolean
diskspace_update(diskspace_priv *priv)
{
    struct statvfs sv;
    guint64  total_b, used_b, free_b;
    gdouble  fraction;
    gchar    tooltip[128];

    ENTER;

    if (statvfs(priv->mountpoint, &sv) != 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(priv->pb), 1.0);
        g_snprintf(tooltip, sizeof(tooltip),
                   "<b>Disk (%s):</b> unavailable", priv->mountpoint);
        gtk_widget_set_tooltip_markup(priv->plugin.pwid, tooltip);
        RET(TRUE);
    }

    total_b = (guint64) sv.f_blocks * sv.f_frsize;
    free_b  = (guint64) sv.f_bfree  * sv.f_frsize;
    used_b  = total_b - free_b;

    fraction = (total_b > 0)
               ? (gdouble) used_b / (gdouble) total_b
               : 0.0;

    DBG("diskspace: %s used=%llu total=%llu frac=%.2f\n",
        priv->mountpoint,
        (unsigned long long) used_b,
        (unsigned long long) total_b,
        fraction);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(priv->pb), fraction);

    /* Express sizes in the largest convenient unit. */
    if (total_b >= (guint64)1 << 30) {
        g_snprintf(tooltip, sizeof(tooltip),
                   "<b>Disk (%s):</b> %d%%\n"
                   "Used: %.1f GiB of %.1f GiB\n"
                   "Free: %.1f GiB",
                   priv->mountpoint,
                   (int)(fraction * 100),
                   (double) used_b  / (1 << 30),
                   (double) total_b / (1 << 30),
                   (double) free_b  / (1 << 30));
    } else {
        g_snprintf(tooltip, sizeof(tooltip),
                   "<b>Disk (%s):</b> %d%%\n"
                   "Used: %.1f MiB of %.1f MiB\n"
                   "Free: %.1f MiB",
                   priv->mountpoint,
                   (int)(fraction * 100),
                   (double) used_b  / (1 << 20),
                   (double) total_b / (1 << 20),
                   (double) free_b  / (1 << 20));
    }
    gtk_widget_set_tooltip_markup(priv->plugin.pwid, tooltip);

    RET(TRUE);
}

/*
 * diskspace_constructor -- initialise the disk space plugin.
 *
 * Reads config, probes the mount point with statvfs(), and creates the
 * GtkProgressBar.  Returns 0 (soft-disable) if the mount point is
 * inaccessible at startup.
 *
 * Returns: 1 on success, 0 on soft-disable.
 */
static int
diskspace_constructor(plugin_instance *p)
{
    diskspace_priv *priv;
    struct statvfs  sv;
    gint            w, h;
    GtkProgressBarOrientation orient;

    ENTER;

    priv = (diskspace_priv *) p;
    priv->mountpoint = "/";
    priv->period     = 10000;

    XCG(p->xc, "MountPoint", &priv->mountpoint, str);
    XCG(p->xc, "Period",     &priv->period,      int);

    if (priv->period < 1000)
        priv->period = 1000;

    if (statvfs(priv->mountpoint, &sv) != 0) {
        g_message("diskspace: statvfs(\"%s\") failed — plugin disabled",
                  priv->mountpoint);
        RET(0);
    }

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

    diskspace_update(priv);
    priv->timer = g_timeout_add(priv->period,
                                (GSourceFunc) diskspace_update, priv);
    RET(1);
}

/*
 * diskspace_destructor -- clean up disk space plugin resources.
 *
 * Cancels the polling timer.  GTK widgets are destroyed by the framework.
 * priv->mountpoint is a non-owning xconf pointer; do not free.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 */
static void
diskspace_destructor(plugin_instance *p)
{
    diskspace_priv *priv = (diskspace_priv *) p;

    ENTER;
    if (priv->timer) {
        g_source_remove(priv->timer);
        priv->timer = 0;
    }
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "diskspace",
    .name        = "Disk Space",
    .version     = "1.0",
    .description = "Display disk space usage via statvfs",
    .priv_size   = sizeof(diskspace_priv),
    .constructor = diskspace_constructor,
    .destructor  = diskspace_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
