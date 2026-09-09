/**
 * @file
 * @brief fbpanel memory usage plugin (scrolling chart style).
 *
 * Like mem.c, but renders memory usage history as a scrolling bar chart
 * via the shared chart plugin helper instead of a plain progress bar.
 * Supports two chart rows: row 0 is RAM usage (default colour red), and
 * row 1 is swap usage (optional; enabled by setting SwapColor). Every
 * CHECK_PERIOD (2) seconds, mem_usage() reads /proc/meminfo, computes
 * fractional usage in [0..1] for each row, calls chart->add_tick(), and
 * updates the tooltip.
 *
 * @par Licence
 *   GPLv2. Contact: bercik-rrp@users.sf.net
 *
 * @par Struct layout (C-style inheritance)
 *   mem2_priv embeds chart_priv as its FIRST member so it can be safely
 *   cast to chart_priv*, and (since chart_priv itself embeds
 *   plugin_instance first) to plugin_instance*.
 *
 * @par Configuration (xconf keys)
 *   - `MemColor`  -- colour string for the RAM row (default "red").
 *   - `SwapColor` -- colour string for the swap row; if absent, swap is
 *     not shown.
 */

#include "../chart/chart.h"
#include <stdlib.h>
#include <string.h>

//#define DEBUGPRN
#include "dbg.h"

#define CHECK_PERIOD   2   /* seconds between memory samples */

/*
 * mem2_priv -- private state for one mem2 plugin instance.
 *
 * chart    - embedded chart helper (MUST be first; casts to plugin_instance*).
 * timer    - GLib timeout source ID.
 * max      - unused maximum value placeholder (reserved for future use).
 * colors   - two-entry color array: [0] RAM color, [1] swap color (or NULL).
 */
typedef struct {
    chart_priv chart;
    int timer;
    gulong max;
    gchar *colors[2];
} mem2_priv;

/* chart_class obtained from class_get("chart"); shared across all mem2 instances. */
static chart_class *k;

static void mem2_destructor(plugin_instance *p);

/*
 * mem_type_t -- one /proc/meminfo field descriptor.
 *
 * name  - field name string matching /proc/meminfo (e.g. "MemTotal").
 * val   - last parsed value in kB.
 * valid - 1 if val has been set for the current parse pass.
 */
typedef struct {
    char *name;
    gulong val;
    int valid;
} mem_type_t;


#if defined __linux__
/* First X-macro pass: generate MT_MemTotal, MT_MemFree, … MT_NUM enum */
#undef MT_ADD
#define MT_ADD(x) MT_ ## x,
enum {
#include "../mem/mt.h"
    MT_NUM   /* sentinel; equals the total number of tracked fields */
};

/* Second X-macro pass: generate mt[] array with name strings */
#undef MT_ADD
#define MT_ADD(x) { #x, 0, 0 },
mem_type_t mt[] =
{
#include "../mem/mt.h"
};

/*
 * mt_match -- try to parse @buf as the /proc/meminfo line for @m.
 *
 * Returns TRUE and sets m->val if the line starts with m->name and
 * contains a parseable integer value after the field name.
 */
static gboolean
mt_match(char *buf, mem_type_t *m)
{
    gulong val;
    int len;

    len = strlen(m->name);
    if (strncmp(buf, m->name, len))
        return FALSE;
    /* /proc/meminfo format: "FieldName:   VALUE kB\n" */
    if (sscanf(buf + len + 1, "%lu", &val) != 1)
        return FALSE;
    m->val = val;
    m->valid = 1;
    DBG("%s: %lu\n", m->name, val);
    return TRUE;
}

/*
 * mem_usage -- read /proc/meminfo and push one tick to the chart.
 *
 * Resets all mt[] entries, reads /proc/meminfo, computes:
 *   total_r[0] = RAM used fraction  = used / MemTotal
 *   total_r[1] = Swap used fraction = used / SwapTotal
 *
 * Then calls k->add_tick() with the two fractions and updates the tooltip.
 *
 * Parameters:
 *   c - mem2_priv instance.
 *
 * Returns: TRUE (keep the timer running) or FALSE on /proc/meminfo open fail.
 */
static int
mem_usage(mem2_priv *c)
{
    FILE *fp;
    char buf[160];
    long unsigned int total[2];
    float total_r[2];
    int i;

    fp = fopen("/proc/meminfo", "r");
    if (!fp)
        RET(FALSE);;   /* double semicolon is harmless */
    /* reset all fields before each parse */
    for (i = 0; i < MT_NUM; i++)
    {
        mt[i].valid = 0;
        mt[i].val = 0;
    }

    while ((fgets(buf, sizeof(buf), fp)) != NULL)
    {
        for (i = 0; i < MT_NUM; i++)
        {
            if (!mt[i].valid && mt_match(buf, mt + i))
                break;
        }
    }
    fclose(fp);

    /* RAM: used = Total - (Free + Buffers + Cached + Slab), values in kB */
    total[0] = (float)(mt[MT_MemTotal].val  - (mt[MT_MemFree].val +
        mt[MT_Buffers].val + mt[MT_Cached].val + mt[MT_Slab].val));
    total[1] = (float)(mt[MT_SwapTotal].val - mt[MT_SwapFree].val);
    total_r[0] = (float)total[0] / mt[MT_MemTotal].val;
    total_r[1] = (float)total[1] / mt[MT_SwapTotal].val;

    /* val >> 10 converts kB → MB */
    g_snprintf(buf, sizeof(buf),
        "<b>Mem:</b> %d%%, %lu MB of %lu MB\n"
        "<b>Swap:</b> %d%%, %lu MB of %lu MB",
        (int)(total_r[0] * 100), total[0] >> 10, mt[MT_MemTotal].val >> 10,
        (int)(total_r[1] * 100), total[1] >> 10, mt[MT_SwapTotal].val >> 10);

    k->add_tick(&c->chart, total_r);   /* push to chart ring-buffer */
    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    RET(TRUE);

}
#else
/* Non-Linux stub — no memory information available.
 * Signature matches the Linux version and GSourceFunc requirements. */
static gboolean
mem_usage(mem2_priv *c)
{
    (void)c;   /* unused on this platform */
    return TRUE;
}
#endif

/**
 * @brief Initialise the mem2 plugin.
 *
 * Acquires the "chart" plugin class and delegates widget construction to
 * its constructor, reads MemColor/SwapColor from xconf, then configures
 * one chart row (RAM only) or two (RAM + swap) depending on whether
 * SwapColor was specified in the config.
 *
 * @param p Plugin instance allocated by the panel framework.
 * @return 1 on success, 0 if the "chart" plugin class is unavailable or
 *         its constructor fails.
 */
static int
mem2_constructor(plugin_instance *p)
{
    mem2_priv *c;

    if (!(k = class_get("chart"))) {   /* obtain shared chart plugin class */
        g_message("mem2: 'chart' plugin unavailable — plugin disabled");
        RET(0);
    }
    if (!PLUGIN_CLASS(k)->constructor(p)) {   /* build chart widget on p->pwid */
        g_message("mem2: chart constructor failed — plugin disabled");
        RET(0);
    }
    c = (mem2_priv *) p;

    c->colors[0] = "red";   /* default RAM colour */
    c->colors[1] = NULL;    /* no swap by default */
    XCG(p->xc, "MemColor",  &c->colors[0], str);
    XCG(p->xc, "SwapColor", &c->colors[1], str);

    if (c->colors[1] == NULL) {
        k->set_rows(&c->chart, 1, c->colors);   /* RAM only */
    } else {
        k->set_rows(&c->chart, 2, c->colors);   /* RAM + swap */
    }
    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid,
        "<b>Memory</b>");
    mem_usage(c);   /* initial sample */
    c->timer = g_timeout_add(CHECK_PERIOD * 1000,
        (GSourceFunc) mem_usage, (gpointer) c);
    RET(1);
}


/**
 * @brief Clean up mem2 plugin resources.
 *
 * Removes the polling timer, calls the chart plugin's destructor to free
 * its tick buffers and GdkGCs, then releases the "chart" class
 * reference.
 *
 * @param p Plugin instance being torn down (cast internally to
 *          mem2_priv*).
 */
static void
mem2_destructor(plugin_instance *p)
{
    mem2_priv *c = (mem2_priv *) p;

    ENTER;
    if (c->timer)
        g_source_remove(c->timer);   /* stop the CHECK_PERIOD poll */
    PLUGIN_CLASS(k)->destructor(p);  /* free ticks and GCs */
    class_put("chart");              /* release chart class reference */
    RET();
}


static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "mem2",
    .name        = "Chart Memory Monitor",
    .version     = "1.0",
    .description = "Show memory usage as chart",
    .priv_size   = sizeof(mem2_priv),

    .constructor = mem2_constructor,
    .destructor  = mem2_destructor,
};
/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
