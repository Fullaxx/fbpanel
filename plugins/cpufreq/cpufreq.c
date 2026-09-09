/**
 * @file
 * @brief fbpanel CPU frequency plugin.
 *
 * Displays the current CPU clock frequency for a configured core as a text
 * label (e.g. "3.40 GHz" or "800 MHz"), read from the cpufreq sysfs
 * interface.
 *
 * @note Soft-disable: if the cpufreq sysfs node for the configured CPU
 *       index does not exist (e.g. a VM, a container, or a CPU without
 *       frequency scaling support), the constructor emits g_message() and
 *       returns 0; the panel skips the plugin and continues loading
 *       normally.
 *
 * @par Configuration (xconf keys)
 *   - CpuIndex -- CPU core index to read (default: 0, meaning cpu0).
 *   - Period   -- update interval in milliseconds (default: 2000).
 *
 * @par Data source
 *   - `/sys/devices/system/cpu/cpu<N>/cpufreq/scaling_cur_freq`
 *   - Value is in kHz. Divided by 1000 to get MHz; by 1000000 to get GHz.
 */

#include <stdio.h>
#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * cpufreq_priv -- per-instance private state.
 *
 * plugin   -- base class (MUST be first).
 * label    -- GtkLabel showing the formatted frequency string.
 * timer    -- GLib timeout source ID; 0 when inactive.
 * cpu_idx  -- CPU core index (0-based).
 * period   -- polling interval in milliseconds.
 * path     -- full sysfs path to scaling_cur_freq; heap-allocated, g_free in dtor.
 */
typedef struct {
    plugin_instance  plugin;
    GtkWidget       *label;
    guint            timer;
    int              cpu_idx;
    int              period;
    gchar           *path;
} cpufreq_priv;

/*
 * cpufreq_update -- read the current CPU frequency and update the label.
 *
 * Reads the sysfs scaling_cur_freq file and formats the result in GHz
 * (if >= 1 GHz) or MHz.  Updates the tooltip with the raw kHz value.
 *
 * Parameters:
 *   priv -- cpufreq_priv instance.
 *
 * Returns: TRUE to keep the GLib timer repeating.
 */
static gboolean
cpufreq_update(cpufreq_priv *priv)
{
    FILE  *f;
    gulong khz = 0;
    gchar  display[32];
    gchar  tooltip[64];

    ENTER;

    f = fopen(priv->path, "r");
    if (!f) {
        gtk_label_set_text(GTK_LABEL(priv->label), "n/a");
        RET(TRUE);
    }
    (void) fscanf(f, "%lu", &khz);
    fclose(f);

    if (khz >= 1000000) {
        g_snprintf(display, sizeof(display), "%.2f GHz",
                   (double) khz / 1000000.0);
    } else {
        g_snprintf(display, sizeof(display), "%lu MHz", khz / 1000);
    }

    gtk_label_set_text(GTK_LABEL(priv->label), display);

    g_snprintf(tooltip, sizeof(tooltip),
               "<b>CPU%d frequency:</b> %lu kHz", priv->cpu_idx, khz);
    gtk_widget_set_tooltip_markup(priv->plugin.pwid, tooltip);

    DBG("cpufreq: cpu%d = %lu kHz (%s)\n", priv->cpu_idx, khz, display);
    RET(TRUE);
}

/**
 * @brief Constructor for the cpufreq plugin.
 *
 * Reads config, constructs the sysfs path, and probes it.
 *
 * @param p plugin_instance* allocated by the fbpanel framework.
 * @return 1 on success, 0 on soft-disable (the sysfs path does not exist).
 */
static int
cpufreq_constructor(plugin_instance *p)
{
    cpufreq_priv *priv;
    FILE         *probe;

    ENTER;

    priv = (cpufreq_priv *) p;
    priv->cpu_idx = 0;
    priv->period  = 2000;

    XCG(p->xc, "CpuIndex", &priv->cpu_idx, int);
    XCG(p->xc, "Period",   &priv->period,   int);

    if (priv->period < 250)
        priv->period = 250;
    if (priv->cpu_idx < 0)
        priv->cpu_idx = 0;

    priv->path = g_strdup_printf(
        "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",
        priv->cpu_idx);

    probe = fopen(priv->path, "r");
    if (!probe) {
        g_message("cpufreq: %s not available — plugin disabled", priv->path);
        g_free(priv->path);
        priv->path = NULL;
        RET(0);
    }
    fclose(probe);

    priv->label = gtk_label_new("...");
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);

    cpufreq_update(priv);
    priv->timer = g_timeout_add(priv->period,
                                (GSourceFunc) cpufreq_update, priv);
    RET(1);
}

/**
 * @brief Destructor for the cpufreq plugin.
 *
 * Cancels the timer and frees the heap-allocated sysfs path.
 *
 * @param p plugin_instance* pointer.
 */
static void
cpufreq_destructor(plugin_instance *p)
{
    cpufreq_priv *priv = (cpufreq_priv *) p;

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
    .type        = "cpufreq",
    .name        = "CPU Frequency",
    .version     = "1.0",
    .description = "Display current CPU clock frequency from cpufreq sysfs",
    .priv_size   = sizeof(cpufreq_priv),
    .constructor = cpufreq_constructor,
    .destructor  = cpufreq_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
