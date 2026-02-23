/*
 * diskio.c -- fbpanel disk I/O monitor plugin.
 *
 * Displays disk read and write throughput as a scrolling strip chart,
 * using the shared "chart" plugin as the rendering backend (same pattern
 * as the cpu and net plugins).
 *
 * Soft-disable behaviour:
 *   If /proc/diskstats cannot be opened at startup, or if the configured
 *   device name is not found in /proc/diskstats, the constructor emits
 *   g_message() and returns 0.  The panel skips the plugin and continues.
 *
 * Configuration (xconf keys):
 *   Device     -- block device name to monitor (default: "sda").
 *   ReadLimit  -- max expected read throughput in KiB/s for chart scale
 *                 (default: 100000).
 *   WriteLimit -- max expected write throughput in KiB/s for chart scale
 *                 (default: 100000).
 *   ReadColor  -- chart colour for reads  (default: "green").
 *   WriteColor -- chart colour for writes (default: "red").
 *
 * Data source:
 *   /proc/diskstats — fields (1-indexed):
 *     1: major  2: minor  3: devname
 *     4: reads_completed  5: reads_merged  6: sectors_read  7: ms_reading
 *     8: writes_completed 9: writes_merged 10: sectors_written 11: ms_writing
 *   Throughput (KiB/s) = delta_sectors * 512 / 1024 / CHECK_PERIOD
 *                      = delta_sectors / (2 * CHECK_PERIOD)
 *
 * Struct layout (C-style inheritance):
 *   diskio_priv embeds chart_priv as its FIRST member, allowing safe cast
 *   to plugin_instance* and chart_priv* (same pattern as cpu_priv, net_priv).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../chart/chart.h"

//#define DEBUGPRN
#include "dbg.h"

/* Sampling interval in seconds. */
#define CHECK_PERIOD 2

/*
 * diskio_stat -- snapshot of cumulative sector counters for one device.
 *
 * read_sectors  -- total sectors read since boot.
 * write_sectors -- total sectors written since boot.
 */
struct diskio_stat {
    gulong read_sectors;
    gulong write_sectors;
};

/*
 * diskio_priv -- per-instance private state.
 *
 * chart       -- embedded chart base class (MUST be first field).
 * prev        -- previous sector snapshot for delta computation.
 * timer       -- GLib timeout source ID; 0 when inactive.
 * device      -- device name to monitor (non-owning xconf pointer).
 * max_read    -- chart normalisation ceiling for reads in KiB/s.
 * max_write   -- chart normalisation ceiling for writes in KiB/s.
 * max         -- max_read + max_write; combined ceiling.
 * colors      -- two-entry colour array for set_rows().
 */
typedef struct {
    chart_priv         chart;   /* MUST be first */
    struct diskio_stat prev;
    int                timer;
    char              *device;
    gint               max_read;
    gint               max_write;
    gulong             max;
    gchar             *colors[2];
} diskio_priv;

/* Module-level chart class pointer, obtained via class_get("chart"). */
static chart_class *k;

/* Forward declaration. */
static void diskio_destructor(plugin_instance *p);

/*
 * diskio_read_stat -- read sector counters for priv->device from /proc/diskstats.
 *
 * Scans /proc/diskstats for a line whose third field matches priv->device
 * and parses fields 6 (sectors_read) and 10 (sectors_written).
 *
 * Parameters:
 *   priv -- diskio_priv instance (priv->device is the target device name).
 *   out  -- output: populated sector counters on success.
 *
 * Returns: 0 on success, -1 on failure (file unreadable or device not found).
 */
static int
diskio_read_stat(diskio_priv *priv, struct diskio_stat *out)
{
    FILE *f;
    char  buf[256];
    char  devname[64];
    int   major, minor;
    gulong rd_ios, rd_mrg, rd_sec, rd_ticks;
    gulong wr_ios, wr_mrg, wr_sec;

    out->read_sectors  = 0;
    out->write_sectors = 0;

    f = fopen("/proc/diskstats", "r");
    if (!f)
        return -1;

    while (fgets(buf, sizeof(buf), f)) {
        /* /proc/diskstats line format (space-separated):
         * major minor devname reads_completed reads_merged sectors_read
         * ms_reading writes_completed writes_merged sectors_written ... */
        if (sscanf(buf, "%d %d %63s %lu %lu %lu %lu %lu %lu %lu",
                   &major, &minor, devname,
                   &rd_ios, &rd_mrg, &rd_sec, &rd_ticks,
                   &wr_ios, &wr_mrg, &wr_sec) == 10) {
            if (strcmp(devname, priv->device) == 0) {
                out->read_sectors  = rd_sec;
                out->write_sectors = wr_sec;
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    return -1;  /* device not found */
}

/*
 * diskio_update -- sample disk counters and push one tick to the chart.
 *
 * Computes the per-interval delta in KiB/s:
 *   delta_read_kib  = (cur_read_sec  - prev_read_sec)  / (2 * CHECK_PERIOD)
 *   delta_write_kib = (cur_write_sec - prev_write_sec) / (2 * CHECK_PERIOD)
 *
 * Normalises to [0.0, 1.0] against priv->max and pushes to the chart.
 *
 * Parameters:
 *   priv -- diskio_priv instance.
 *
 * Returns: TRUE to keep the GLib timer alive.
 */
static int
diskio_update(diskio_priv *priv)
{
    struct diskio_stat cur;
    gulong delta_r, delta_w;
    float  total[2];
    gchar  tooltip[128];

    ENTER;

    total[0] = total[1] = 0.0f;

    if (diskio_read_stat(priv, &cur) != 0)
        goto push;

    /* Sectors are 512 bytes; divide by 2 to convert to KiB.
     * Then divide by CHECK_PERIOD to get KiB/s. */
    delta_r = ((cur.read_sectors  - priv->prev.read_sectors)  / 2)
              / CHECK_PERIOD;
    delta_w = ((cur.write_sectors - priv->prev.write_sectors) / 2)
              / CHECK_PERIOD;

    priv->prev = cur;

    if (priv->max > 0) {
        total[0] = (float) delta_r / (float) priv->max;
        total[1] = (float) delta_w / (float) priv->max;
    }

    DBG("diskio: %s read=%lu write=%lu KiB/s\n",
        priv->device, delta_r, delta_w);

    g_snprintf(tooltip, sizeof(tooltip),
               "<b>%s:</b>\nRead:  %lu KiB/s\nWrite: %lu KiB/s",
               priv->device, delta_r, delta_w);
    gtk_widget_set_tooltip_markup(((plugin_instance *)priv)->pwid, tooltip);

push:
    k->add_tick(&priv->chart, total);
    RET(TRUE);
}

/*
 * diskio_constructor -- initialise the disk I/O plugin.
 *
 * Acquires the chart helper class, reads config, probes /proc/diskstats
 * for the configured device, and starts the sampling timer.
 *
 * Returns: 1 on success, 0 on soft-disable.
 */
static int
diskio_constructor(plugin_instance *p)
{
    diskio_priv       *priv;
    struct diskio_stat probe;

    if (!(k = class_get("chart"))) {
        g_message("diskio: 'chart' plugin unavailable — plugin disabled");
        RET(0);
    }
    if (!PLUGIN_CLASS(k)->constructor(p)) {
        g_message("diskio: chart constructor failed — plugin disabled");
        RET(0);
    }

    priv = (diskio_priv *) p;

    priv->device      = "sda";
    priv->max_read    = 100000;
    priv->max_write   = 100000;
    priv->colors[0]   = "green";
    priv->colors[1]   = "red";

    XCG(p->xc, "Device",     &priv->device,    str);
    XCG(p->xc, "ReadLimit",  &priv->max_read,  int);
    XCG(p->xc, "WriteLimit", &priv->max_write, int);
    XCG(p->xc, "ReadColor",  &priv->colors[0], str);
    XCG(p->xc, "WriteColor", &priv->colors[1], str);

    priv->max = (gulong) priv->max_read + (gulong) priv->max_write;

    /* Probe: verify /proc/diskstats is readable and device exists. */
    if (diskio_read_stat(priv, &probe) != 0) {
        g_message("diskio: device '%s' not found in /proc/diskstats"
                  " — plugin disabled", priv->device);
        /* Must tear down the chart before returning 0. */
        PLUGIN_CLASS(k)->destructor(p);
        class_put("chart");
        RET(0);
    }
    priv->prev = probe;

    k->set_rows(&priv->chart, 2, priv->colors);
    gtk_widget_set_tooltip_markup(((plugin_instance *)priv)->pwid,
                                  "<b>Disk I/O</b>");

    diskio_update(priv);
    priv->timer = g_timeout_add(CHECK_PERIOD * 1000,
                                (GSourceFunc) diskio_update, priv);
    RET(1);
}

/*
 * diskio_destructor -- clean up disk I/O plugin resources.
 *
 * Cancels the timer, tears down the chart, and releases the chart class.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 */
static void
diskio_destructor(plugin_instance *p)
{
    diskio_priv *priv = (diskio_priv *) p;

    ENTER;
    if (priv->timer) {
        g_source_remove(priv->timer);
        priv->timer = 0;
    }
    PLUGIN_CLASS(k)->destructor(p);
    class_put("chart");
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "diskio",
    .name        = "Disk I/O",
    .version     = "1.0",
    .description = "Display disk read/write throughput from /proc/diskstats",
    .priv_size   = sizeof(diskio_priv),
    .constructor = diskio_constructor,
    .destructor  = diskio_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
