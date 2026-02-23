/*
 * cpu.c -- fbpanel CPU usage chart plugin.
 *
 * Displays CPU utilisation as a scrolling bar chart (using the shared
 * chart plugin as a helper).  One row, coloured green by default.
 *
 * Platform support:
 *   Linux   — reads /proc/stat every 1000 ms.
 *   FreeBSD — reads kern.cp_time sysctl every 1000 ms.
 *   Other   — stub that always reports 0% (non-functional).
 *
 * Struct layout (C-style inheritance):
 *   cpu_priv embeds chart_priv as its FIRST member so that a cpu_priv*
 *   can be safely cast to chart_priv* and plugin_instance* (the chart
 *   plugin, in turn, embeds plugin_instance as its first member).
 *
 * Timer:
 *   cpu_get_load() is called once from the constructor and then every
 *   1000 ms via g_timeout_add().  It computes the δ between successive
 *   /proc/stat readings, passes the normalised fraction [0..1] to
 *   chart->add_tick(), and updates the tooltip.
 *
 * Known bugs:
 *   BUG: In the non-Linux/FreeBSD stub, cpu_get_load_real() references
 *     the parameter as "cpu" but the parameter is named "s" → compile error.
 *   BUG: If cpu_get_load_real() fails (goto end), the local variables
 *     "a" and "b" are used uninitialised in the DBG() trace after the
 *     label.  total[0] remains 0.0 (from memset) which is fine, but the
 *     debug print reads uninitialized stack values.
 */

#include <string.h>
#include "misc.h"
#include "../chart/chart.h"

//#define DEBUGPRN
#include "dbg.h"
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#endif

/* Cumulative CPU time counters from the OS */
struct cpu_stat {
    gulong u, n, s, i, w; /* user, nice, system, idle, wait (iowait) */
};

/*
 * cpu_priv — private state for one CPU plugin instance.
 *
 * chart    - embedded chart helper (MUST be first; casts to plugin_instance*).
 * cpu_prev - previous cpu_stat snapshot (used to compute delta each tick).
 * timer    - GLib timeout source ID (for g_source_remove in destructor).
 * colors   - single-entry color array passed to chart->set_rows().
 */
typedef struct {
    chart_priv chart;
    struct cpu_stat cpu_prev;
    int timer;
    gchar *colors[1];
} cpu_priv;

/* chart_class obtained from class_get("chart"); shared across all cpu instances. */
static chart_class *k;

static void cpu_destructor(plugin_instance *p);


/*
 * cpu_get_load_real -- platform-specific CPU counter reader.
 *
 * Linux:   reads the first "cpu" line of /proc/stat.
 *          Fields: user nice system idle iowait (irq softirq are ignored).
 * FreeBSD: reads kern.cp_time via sysctl.
 *          mib[] is cached after first lookup (init flag).
 * Other:   stub — zeroes the struct and returns 0 (always 0% CPU).
 *
 * Returns: 0 on success, -1 on failure.
 *
 * BUG (non-Linux/FreeBSD stub): the parameter name is "s" but the body
 *   references "cpu" — compile-time error on unsupported platforms.
 */
#if defined __linux__
static int
cpu_get_load_real(struct cpu_stat *cpu)
{
    FILE *stat;

    memset(cpu, 0, sizeof(struct cpu_stat));
    stat = fopen("/proc/stat", "r");
    if(!stat)
        return -1;

    /* first line format: "cpu  user nice system idle iowait ..." */
    (void)fscanf(stat, "cpu %lu %lu %lu %lu %lu", &cpu->u, &cpu->n, &cpu->s,
            &cpu->i, &cpu->w);
    fclose(stat);

    return 0;
}
#elif defined __FreeBSD__
static int
cpu_get_load_real(struct cpu_stat *cpu)
{
    static int mib[2] = { -1, -1 }, init = 0;
    size_t j;
    long ct[CPUSTATES];

    memset(cpu, 0, sizeof(struct cpu_stat));
    if (init == 0) {
        j = 2;
        if (sysctlnametomib("kern.cp_time", mib, &j) != 0)
            return -1;   /* sysctl name not found */

        init = 1;
    }

    j = sizeof(ct);
    if (sysctl(mib, 2, ct, &j, NULL, 0) != 0)
        return -1;
    cpu->u = ct[CP_USER];
    cpu->n = ct[CP_NICE];
    cpu->s = ct[CP_SYS];
    cpu->i = ct[CP_IDLE];
    cpu->w = 0;   /* FreeBSD has no iowait counter */

    return 0;
}
#else
/* BUG: parameter is named "s" but body would need "cpu" — compile error */
static int
cpu_get_load_real(struct cpu_stat *s)
{
    memset(cpu, 0, sizeof(struct cpu_stat));
    return 0;
}
#endif

/*
 * cpu_get_load -- compute CPU load fraction and push it to the chart.
 *
 * Reads current cumulative CPU counters, subtracts previous snapshot,
 * and computes active / total fraction.  Passes float[1] to add_tick().
 * Also updates the plugin tooltip with the percentage.
 *
 * Called once immediately from cpu_constructor, then every 1000 ms by timer.
 *
 * Parameters:
 *   c - cpu_priv instance (cast-compatible with plugin_instance*).
 *
 * Returns: TRUE (keeps the GLib timer running).
 *
 * BUG: if cpu_get_load_real() fails (goto end), locals "a" and "b" are
 *   used uninitialised in the DBG() call after the label.  total[0]
 *   stays 0.0, which is the right value to report in that case, so the
 *   chart behaviour is safe — only the debug trace is affected.
 */
static int
cpu_get_load(cpu_priv *c)
{
    gfloat a, b;
    struct cpu_stat cpu, cpu_diff;
    float total[1];   /* single-row value for add_tick */
    gchar buf[40];

    ENTER;
    memset(&cpu, 0, sizeof(cpu));
    memset(&cpu_diff, 0, sizeof(cpu_diff));
    memset(&total, 0, sizeof(total));

    if (cpu_get_load_real(&cpu))
        goto end;   /* BUG: a, b uninitialised past this point */

    /* compute per-field deltas since last sample */
    cpu_diff.u = cpu.u - c->cpu_prev.u;
    cpu_diff.n = cpu.n - c->cpu_prev.n;
    cpu_diff.s = cpu.s - c->cpu_prev.s;
    cpu_diff.i = cpu.i - c->cpu_prev.i;
    cpu_diff.w = cpu.w - c->cpu_prev.w;
    c->cpu_prev = cpu;   /* save for next tick */

    /* active = user + nice + system; total = active + idle + iowait */
    a = cpu_diff.u + cpu_diff.n + cpu_diff.s;
    b = a + cpu_diff.i + cpu_diff.w;
    total[0] = b ? a / b : 1.0;   /* avoid division by zero; assume 100% if b=0 */

end:
    DBG("total=%f a=%f b=%f\n", total[0], a, b);  /* BUG: a,b uninit if goto taken */
    g_snprintf(buf, sizeof(buf), "<b>Cpu:</b> %d%%", (int)(total[0] * 100));
    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    k->add_tick(&c->chart, total);   /* push to chart ring-buffer */
    RET(TRUE);

}

/*
 * cpu_constructor -- initialise the CPU plugin.
 *
 * Acquires the chart helper class, delegates widget construction to
 * chart_constructor (via PLUGIN_CLASS(k)->constructor), configures one
 * row coloured green (or from config "Color" key), starts the 1000 ms
 * polling timer.
 *
 * Parameters:
 *   p - plugin_instance allocated by the panel framework.
 *
 * Returns: 1 on success, 0 on failure (chart class unavailable).
 */
static int
cpu_constructor(plugin_instance *p)
{
    cpu_priv *c;

    if (!(k = class_get("chart")))   /* obtain shared chart plugin class */
        RET(0);
    if (!PLUGIN_CLASS(k)->constructor(p))   /* build chart widget on p->pwid */
        RET(0);
    c = (cpu_priv *) p;
    c->colors[0] = "green";
    XCG(p->xc, "Color", &c->colors[0], str);   /* optional config override */

    k->set_rows(&c->chart, 1, c->colors);   /* 1 row = total CPU usage */
    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, "<b>Cpu</b>");
    cpu_get_load(c);   /* initial sample (sets cpu_prev) */
    c->timer = g_timeout_add(1000, (GSourceFunc) cpu_get_load, (gpointer) c);
    RET(1);
}


/*
 * cpu_destructor -- clean up CPU plugin resources.
 *
 * Removes the polling timer, calls the chart destructor to free tick
 * buffers and GdkGCs, then releases the chart class reference.
 *
 * Parameters:
 *   p - plugin_instance.
 */
static void
cpu_destructor(plugin_instance *p)
{
    cpu_priv *c = (cpu_priv *) p;

    ENTER;
    g_source_remove(c->timer);          /* stop the 1000 ms poll */
    PLUGIN_CLASS(k)->destructor(p);     /* free ticks and GCs */
    class_put("chart");                 /* release chart class reference */
    RET();
}



static plugin_class class = {
    .count       = 0,
    .type        = "cpu",
    .name        = "Cpu usage",
    .version     = "1.0",
    .description = "Display cpu usage",
    .priv_size   = sizeof(cpu_priv),
    .constructor = cpu_constructor,
    .destructor  = cpu_destructor,
};

/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
