/*
 * plugins/net/net.c -- Network traffic monitor plugin for fbpanel.
 *
 * A little bug fixed by Mykola <mykola@2ka.mipt.ru>:)
 * FreeBSD support is added by Eygene Ryabinkin <rea-fbsd@codelabs.ru>
 *
 * PURPOSE
 * -------
 * Displays a scrolling chart of network transmit (TX) and receive (RX)
 * traffic in KiB/s for a single network interface.  The chart is rendered
 * by the "chart" plugin (plugins/chart/), which this plugin uses as a
 * subordinate/base class.
 *
 * PLATFORM SUPPORT
 * ----------------
 * Two platform backends are compiled conditionally:
 *   Linux   -- reads /proc/net/dev for byte counters.
 *   FreeBSD -- uses sysctl(KERN_NET / IFMIB) to query ifmibdata.
 *
 * POLLING
 * -------
 * A GLib timer fires every CHECK_PERIOD seconds.  On each tick, the
 * per-interval byte delta is computed, converted to KiB/s, and clamped to
 * the configured max rate before being added to the chart.
 *
 * CONFIGURATION (xconf keys under the plugin node)
 * -------------------------------------------------
 *   interface  -- interface name (default: "eth0")
 *   RxLimit    -- max expected RX rate in KiB/s (default: 120)
 *   TxLimit    -- max expected TX rate in KiB/s (default: 12)
 *   TxColor    -- chart colour for TX (default: "violet")
 *   RxColor    -- chart colour for RX (default: "blue")
 *
 * PUBLIC API
 * ----------
 *   Exported to the panel loader via the file-scope `class_ptr` variable.
 */

#include "../chart/chart.h"
#include <stdlib.h>
#include <string.h>

//#define DEBUGPRN
#include "dbg.h"

/* Platform-specific headers for FreeBSD network statistics. */
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/if_mib.h>
#endif


/* How often (in seconds) network statistics are sampled. */
#define CHECK_PERIOD   2 /* second */

/*
 * net_stat -- snapshot of cumulative TX/RX byte counters for an interface.
 *
 * Values are read directly from /proc/net/dev (Linux) or sysctl IFMIB
 * (FreeBSD).  The counters are monotonically increasing (wrapping at
 * the platform's gulong maximum).
 *
 * Fields:
 *   tx -- cumulative bytes transmitted since interface came up.
 *   rx -- cumulative bytes received since interface came up.
 *
 * FIXME: gulong is 32 bits on 32-bit platforms.  High-speed interfaces
 *        can wrap the counter in under an hour.  The subtraction in
 *        net_get_load() does not handle wrapping, so a counter rollover
 *        will produce a huge spurious spike.
 */
struct net_stat {
    gulong tx; /* cumulative transmit bytes */
    gulong rx; /* cumulative receive bytes */
};

/*
 * net_priv -- per-instance private state for the net plugin.
 *
 * Inherits chart_priv by embedding it as the FIRST field.  chart_priv in
 * turn embeds plugin_instance as its first field, so a net_priv* can be
 * cast to plugin_instance* or chart_priv* safely.
 *
 * Ownership notes:
 *   iface    -- string from xconf (non-owning) or the literal "eth0" static
 *               string.  NOT heap-allocated; do not g_free().
 *   colors[] -- same: either a static literal or an xconf-owned string.
 *   timer    -- GLib timeout source ID; 0 when inactive.
 */
typedef struct {
    /* Base class (chart plugin state + plugin_instance).
     * MUST be first field -- net_priv* is cast to chart_priv* and
     * plugin_instance* throughout. */
    chart_priv chart;

    /* Byte counters from the previous sample, used to compute the delta. */
    struct net_stat net_prev;

    /* GLib timeout source ID for the periodic CHECK_PERIOD sampling timer.
     * 0 when no timer is active. */
    int timer;

    /* Network interface name to monitor (e.g. "eth0", "wlan0").
     * Non-owning pointer into xconf or a static string literal. */
    char *iface;

#if defined(__FreeBSD__)
    /* Row index in the IFMIB table corresponding to `iface`.
     * 0 means the interface was not found during init_net_stats(). */
    size_t ifmib_row;
#endif

    /* Configured maximum TX rate in KiB/s; used to normalise the chart. */
    gint max_tx;

    /* Configured maximum RX rate in KiB/s; used to normalise the chart. */
    gint max_rx;

    /* Sum of max_tx + max_rx; the combined ceiling for chart normalisation.
     * Computed once in net_constructor(). */
    gulong max;

    /* Chart row colour strings for TX (index 0) and RX (index 1).
     * Non-owning pointers (xconf or static literals). */
    gchar *colors[2];
} net_priv;

/* Pointer to the "chart" plugin class, obtained via class_get("chart").
 * Used to call chart virtual functions (add_tick, set_rows) and
 * PLUGIN_CLASS(k)->constructor/destructor. */
static chart_class *k;


/* Forward declaration: needed because net_constructor references it. */
static void net_destructor(plugin_instance *p);


/*
 * ============================================================
 *  Platform-specific: Linux backend
 * ============================================================
 */
#if defined __linux__

/*
 * init_net_stats -- Linux stub; no initialisation needed.
 *
 * On Linux, /proc/net/dev is parsed directly, so no per-interface
 * identification step is required.  The macro expands to nothing.
 */
#define init_net_stats(x)

/*
 * net_get_load_real -- read current cumulative TX/RX byte counters on Linux.
 *
 * Opens /proc/net/dev, skips the two header lines, then scans for a line
 * containing the interface name string (c->iface).  The relevant fields
 * (receive bytes and transmit bytes) are extracted via sscanf.
 *
 * Parameters:
 *   c   -- net_priv instance (c->iface is the interface name to look up).
 *   net -- output: populated with rx and tx byte counters on success.
 *
 * Returns:
 *    0 -- success; net->rx and net->tx are valid.
 *   -1 -- failure (file not openable, interface not found, parse error).
 *
 * Memory notes:
 *   buf is stack-allocated.  The FILE* stat is always closed before return.
 *
 * BUG: g_strrstr(buf, c->iface) matches the interface name anywhere in the
 *      line, not necessarily at the field boundary.  For example, monitoring
 *      "eth0" would also match a line for "veth0" or "peth0" because the
 *      search does not anchor to the start of the interface-name field.
 *      The correct approach would be to strip leading whitespace and compare
 *      from the line start up to the colon.
 *
 * BUG: The sscanf format string skips 7 intermediate fields between rx bytes
 *      and tx bytes using %*d.  This is correct for the standard /proc/net/dev
 *      format (rx_bytes rx_packets rx_errors rx_drop rx_fifo rx_frame rx_compressed
 *      rx_multicast tx_bytes), but the format can differ across kernel versions
 *      and the parsing is fragile.
 */
static int
net_get_load_real(net_priv *c, struct net_stat *net)
{
    FILE *stat;
    char buf[256], *s = NULL;

    stat = fopen("/proc/net/dev", "r");
    if(!stat)
        return -1;

    /* Skip the two header lines. */
    (void)fgets(buf, 256, stat);
    (void)fgets(buf, 256, stat);

    /* Find the line containing the interface name. */
    while (!s && !feof(stat) && fgets(buf, 256, stat))
        s = g_strrstr(buf, c->iface); /* BUG: may match sub-strings of the name */
    fclose(stat);
    if (!s)
        return -1;

    /* Find the colon separating interface name from statistics. */
    s = g_strrstr(s, ":");
    if (!s)
        return -1;

    s++; /* advance past colon to the start of the numeric fields */

    /* Parse: rx_bytes (skip 7 fields) tx_bytes */
    if (sscanf(s,
            "%lu  %*d     %*d  %*d  %*d  %*d   %*d        %*d       %lu",
            &net->rx, &net->tx) != 2) {
        DBG("can't read %s statistics\n", c->iface);
        return -1;
    }
    return 0;
}

/*
 * ============================================================
 *  Platform-specific: FreeBSD backend
 * ============================================================
 */
#elif defined(__FreeBSD__)

/*
 * init_net_stats -- FreeBSD: locate the IFMIB row for c->iface.
 *
 * Queries the sysctl IFMIB_IFCOUNT to discover how many interfaces are
 * registered, then iterates the IFMIB_IFDATA table to find the row whose
 * ifmd_name matches c->iface.  The matching row index is stored in
 * c->ifmib_row (1-based).  If the interface is not found, c->ifmib_row
 * remains 0, causing net_get_load_real() to return -1 on every call.
 *
 * Parameters:
 *   c -- net_priv instance; c->iface is the interface to locate;
 *        c->ifmib_row is set on success.
 *
 * Side effects:
 *   Sets c->ifmib_row.
 */
static void
init_net_stats(net_priv *c)
{
    int mib[6] = {
        CTL_NET,
        PF_LINK,
        NETLINK_GENERIC,
        IFMIB_SYSTEM,
        IFMIB_IFCOUNT
    };
    u_int count = 0;
    struct ifmibdata ifmd;
    size_t len = sizeof(count);

    c->ifmib_row = 0;
    if (sysctl(mib, 5, (void *)&count, &len, NULL, 0) != 0)
        return; /* sysctl failed; ifmib_row stays 0 */

    /* Switch to the IFDATA subtree for per-interface lookup. */
    mib[3] = IFMIB_IFDATA;
    mib[5] = IFDATA_GENERAL;
    len = sizeof(ifmd);

    for (mib[4] = 1; mib[4] <= count; mib[4]++) {
        if (sysctl(mib, 6, (void *)&ifmd, &len, NULL, 0) != 0)
            continue;
        if (strcmp(ifmd.ifmd_name, c->iface) == 0) {
            c->ifmib_row = mib[4]; /* found; store the 1-based row index */
            break;
        }
    }
}

/*
 * net_get_load_real -- FreeBSD: read current TX/RX byte counters via sysctl.
 *
 * Uses the pre-computed c->ifmib_row to directly query the IFMIB row for
 * the monitored interface.
 *
 * Parameters:
 *   c   -- net_priv instance with a valid c->ifmib_row.
 *   net -- output: populated with rx and tx byte counters on success.
 *
 * Returns:
 *    0 -- success.
 *   -1 -- c->ifmib_row is 0 (interface not found) or sysctl failed.
 */
static int
net_get_load_real(net_priv *c, struct net_stat *net)
{
    int mib[6] = {
        CTL_NET,
        PF_LINK,
        NETLINK_GENERIC,
        IFMIB_IFDATA,
        c->ifmib_row,
        IFDATA_GENERAL
    };
    struct ifmibdata ifmd;
    size_t len = sizeof(ifmd);

    if (!c->ifmib_row)
        return -1; /* interface was not found during initialisation */

    if (sysctl(mib, sizeof(mib)/sizeof(mib[0]), &ifmd, &len, NULL, 0) != 0)
        return -1;

    net->tx = ifmd.ifmd_data.ifi_obytes; /* output bytes */
    net->rx = ifmd.ifmd_data.ifi_ibytes; /* input bytes */
    return 0;
}

#endif /* platform switch */

/*
 * net_get_load -- sample network counters and push one tick to the chart.
 *
 * Called by the GLib timer every CHECK_PERIOD seconds (also called once
 * immediately from net_constructor to prime the chart).
 *
 * Computes the per-interval delta in KiB/s:
 *   delta_tx = (cur_tx - prev_tx) >> 10 / CHECK_PERIOD
 *   delta_rx = (cur_rx - prev_rx) >> 10 / CHECK_PERIOD
 *
 * Normalises to [0.0, 1.0] against c->max and calls chart->add_tick().
 * Updates the tooltip with the current rates.
 *
 * Parameters:
 *   c -- net_priv* cast from gpointer by the timer callback.
 *
 * Returns:
 *   TRUE -- always (keeps the GLib timer active).
 *
 * Memory notes:
 *   buf is stack-allocated.
 *   c->net_prev is updated in-place.
 *
 * BUG: net_diff.tx and net_diff.rx are declared as gulong (unsigned), but
 *      the intermediate computation (net.tx - c->net_prev.tx) >> 10 may wrap
 *      silently if the counter rolls over.  No wrap-detection or clamping is
 *      performed.  A counter wrap will produce an enormous but unsigned value,
 *      which when cast to float and divided by c->max will produce a chart
 *      spike of 1.0 (clamped by the chart renderer, if it clamps).
 *
 * BUG: The format specifier in the g_snprintf tooltip uses "%ul" instead of
 *      "%lu" for gulong.  On most compilers "%ul" is interpreted as "%u"
 *      (unsigned int) followed by the literal character 'l', silently
 *      truncating 64-bit values on 64-bit platforms.  The correct format for
 *      gulong is "%lu".
 *      (The DBG line also uses "%ul" for the same values.)
 *
 * BUG: total[] is a float[2] but memset is called with sizeof(total) which is
 *      sizeof(float[2]) -- this is correct, but using memset on floats is only
 *      valid for zeroing because the bit pattern for +0.0 happens to be all
 *      zero bytes in IEEE 754.  This is technically implementation-defined.
 */
static int
net_get_load(net_priv *c)
{
    struct net_stat net, net_diff;
    float total[2];
    char buf[256];

    ENTER;
    memset(&net, 0, sizeof(net));
    memset(&net_diff, 0, sizeof(net_diff));
    memset(&total, 0, sizeof(total)); /* relies on IEEE 754 zero = all-zero bytes */

    if (net_get_load_real(c, &net))
        goto end; /* platform read failed; push zeros to chart */

    /* Compute per-interval rates in KiB/s.
     * >> 10 converts bytes to KiB; / CHECK_PERIOD normalises per second. */
    net_diff.tx = ((net.tx - c->net_prev.tx) >> 10) / CHECK_PERIOD;
    net_diff.rx = ((net.rx - c->net_prev.rx) >> 10) / CHECK_PERIOD;

    /* Update the rolling baseline for the next sample. */
    c->net_prev = net;

    /* Normalise to [0.0, 1.0] relative to the configured maximum.
     * total[0] = TX fraction, total[1] = RX fraction. */
    total[0] = (float)(net_diff.tx) / c->max;
    total[1] = (float)(net_diff.rx) / c->max;

end:
    /* BUG: "%ul" should be "%lu" for gulong. */
    DBG("%f %f %ul %ul\n", total[0], total[1], net_diff.tx, net_diff.rx);
    k->add_tick(&c->chart, total); /* push data point to the chart widget */

    /* Update tooltip with current rates.
     * BUG: format uses "%lu" here which is correct; the DBG line above is wrong. */
    g_snprintf(buf, sizeof(buf), "<b>%s:</b>\nD %lu Kbs, U %lu Kbs",
        c->iface, net_diff.rx, net_diff.tx);
    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    RET(TRUE); /* keep the GLib timer alive */
}

/*
 * net_constructor -- plugin_class.constructor for the net plugin.
 *
 * Obtains the "chart" plugin class, invokes its constructor to set up the
 * chart widget, then reads net-specific configuration from xconf.  Starts
 * the periodic sampling timer.
 *
 * Parameters:
 *   p -- plugin_instance* allocated by the panel framework.
 *
 * Returns:
 *   1 on success.
 *   0 on failure (chart class unavailable or chart constructor failed).
 *
 * Side effects:
 *   - Sets k (module-level chart_class* pointer).
 *   - Populates c->iface, c->max_rx, c->max_tx, c->colors, c->max.
 *   - Calls init_net_stats() to locate the interface (FreeBSD only).
 *   - Sets up the chart with two rows (TX, RX).
 *   - Starts a repeating GLib timer for net_get_load.
 *   - Calls net_get_load() once immediately to prime the chart.
 *
 * Memory notes:
 *   c->iface and c->colors[] are non-owning pointers to static strings or
 *   xconf-owned strings.  Do NOT g_free() them.
 *
 * BUG: c->iface defaults to the string literal "eth0", which may not exist
 *      on the current system.  No warning is emitted if the interface is
 *      missing.  The chart will simply show zero traffic silently.
 *
 * FIXME: There is no failure path for net_get_load() returning -1 at startup;
 *        the plugin initialises successfully even if the interface does not
 *        exist, showing a silent empty chart.
 */
static int
net_constructor(plugin_instance *p)
{
    net_priv *c;

    /* Load the chart plugin class; it is used as the rendering backend. */
    if (!(k = class_get("chart"))) {
        g_message("net: 'chart' plugin unavailable — plugin disabled");
        RET(0);
    }
    /* Let the chart plugin initialise the base widget. */
    if (!PLUGIN_CLASS(k)->constructor(p)) {
        g_message("net: chart constructor failed — plugin disabled");
        RET(0);
    }

    c = (net_priv *) p;

    /* Set defaults; overridden by xconf keys below. */
    c->iface     = "eth0";   /* BUG: may not exist; no warning on missing iface */
    c->max_rx    = 120;      /* max expected RX KiB/s */
    c->max_tx    = 12;       /* max expected TX KiB/s */
    c->colors[0] = "violet"; /* TX colour */
    c->colors[1] = "blue";   /* RX colour */

    /* Override defaults from plugin configuration. */
    XCG(p->xc, "interface", &c->iface,     str);
    XCG(p->xc, "RxLimit",   &c->max_rx,    int);
    XCG(p->xc, "TxLimit",   &c->max_tx,    int);
    XCG(p->xc, "TxColor",   &c->colors[0], str);
    XCG(p->xc, "RxColor",   &c->colors[1], str);

    /* FreeBSD: look up the IFMIB row for the interface. */
    init_net_stats(c);

    /* Combined ceiling for chart normalisation. */
    c->max = c->max_rx + c->max_tx;

    /* Configure chart with 2 rows: TX and RX. */
    k->set_rows(&c->chart, 2, c->colors);

    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, "<b>Net</b>");

    /* Prime the chart with an initial sample before the timer fires. */
    net_get_load(c);

    /* Start periodic sampling every CHECK_PERIOD seconds. */
    c->timer = g_timeout_add(CHECK_PERIOD * 1000,
        (GSourceFunc) net_get_load, (gpointer) c);
    RET(1);
}

/*
 * net_destructor -- plugin_class.destructor for the net plugin.
 *
 * Cancels the sampling timer and tears down the chart plugin.
 *
 * Parameters:
 *   p -- plugin_instance* being destroyed.
 *
 * Side effects:
 *   - Removes the GLib timer (c->timer).
 *   - Calls chart plugin destructor via PLUGIN_CLASS(k)->destructor(p).
 *   - Releases the chart class reference via class_put("chart").
 *
 * Memory notes:
 *   c->iface and c->colors[] are non-owning; not freed here.
 */
static void
net_destructor(plugin_instance *p)
{
    net_priv *c = (net_priv *) p;

    ENTER;
    if (c->timer)
        g_source_remove(c->timer); /* cancel periodic sampling */

    /* Tear down chart state and widgets. */
    PLUGIN_CLASS(k)->destructor(p);

    /* Release the chart plugin class reference. */
    class_put("chart");
    RET();
}


/*
 * class -- static plugin descriptor for the "net" plugin.
 */
static plugin_class class = {
    .count       = 0,
    .type        = "net",
    .name        = "Net usage",
    .version     = "1.0",
    .description = "Display net usage",
    .priv_size   = sizeof(net_priv),

    .constructor = net_constructor,
    .destructor  = net_destructor,
};

/* class_ptr -- exported symbol used by the panel plugin loader. */
static plugin_class *class_ptr = (plugin_class *) &class;
