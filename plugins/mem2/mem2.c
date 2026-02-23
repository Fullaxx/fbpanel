/*
 * mem2.c -- fbpanel memory usage plugin (chart style).
 *
 * Licence: GPLv2
 *
 * bercik-rrp@users.sf.net
 *
 * Like mem.c but renders usage history as a scrolling bar chart via the
 * shared chart plugin helper.  Supports two chart rows:
 *   Row 0 — RAM usage   (default colour: red).
 *   Row 1 — Swap usage  (optional; enabled by setting SwapColor).
 *
 * Struct layout (C-style inheritance):
 *   mem2_priv embeds chart_priv as FIRST member so it can be safely cast
 *   to chart_priv* and plugin_instance*.
 *
 * Configuration (xconf keys):
 *   MemColor  — colour string for RAM row (default "red").
 *   SwapColor — colour string for swap row; if absent, swap is not shown.
 *
 * Timer:
 *   mem_usage() is called every CHECK_PERIOD (2) seconds via g_timeout_add().
 *   It reads /proc/meminfo, computes fractional usage [0..1] for each row,
 *   calls chart->add_tick(), and updates the tooltip.
 *
 * Known bugs:
 *   BUG: The non-Linux stub for mem_usage() is declared as
 *     "static int mem_usage()" with no parameters, but the Linux version
 *     has signature "static int mem_usage(mem2_priv *c)".  The g_timeout_add
 *     callback passes mem2_priv* as the first argument — correct for the
 *     Linux version, but the non-Linux stub ignores all parameters and
 *     returns nothing (implicit void vs declared int).  This causes a
 *     compile-time type mismatch on non-Linux platforms.
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
/*
 * Non-Linux stub — no memory information available.
 *
 * BUG: declared as returning int with no parameters, but the Linux version
 *   takes mem2_priv* and is called as (GSourceFunc) mem_usage with a
 *   mem2_priv* argument.  This creates a type mismatch on non-Linux builds.
 */
static int
mem_usage()
{
    /* nothing to do on unsupported platforms */
}
#endif

/*
 * mem2_constructor -- initialise the mem2 plugin.
 *
 * Acquires the chart class, delegates widget construction to chart_constructor,
 * reads colour config, then configures 1 or 2 chart rows depending on
 * whether SwapColor was specified in the config.
 *
 * Parameters:
 *   p - plugin_instance allocated by the panel framework.
 *
 * Returns: 1 on success, 0 if chart class unavailable.
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


/*
 * mem2_destructor -- clean up mem2 plugin resources.
 *
 * Removes the polling timer, calls the chart destructor to free tick
 * buffers and GdkGCs, then releases the chart class reference.
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
