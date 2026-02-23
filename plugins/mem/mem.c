/*
 * mem.c -- fbpanel memory usage plugin (progress-bar style).
 *
 * Displays RAM (and optionally swap) usage as vertical (horizontal panel)
 * or horizontal (vertical panel) GtkProgressBar widgets.
 *
 * On Linux, reads /proc/meminfo every 3000 ms using the X-macro expansion
 * of mt.h to generate both the MT_* enum constants and the mt[] array in
 * one step.  "Used" RAM = MemTotal - (MemFree + Buffers + Cached + Slab).
 *
 * Configuration (xconf keys):
 *   ShowSwap — boolean; if "true", a second progress bar for swap is shown.
 *
 * Widget hierarchy:
 *   p->pwid → mem->box (panel's my_box_new) → mem_pb [+ swap_pb]
 *
 * Fixed bugs:
 *   Fixed (BUG-004): Removed explicit gtk_widget_destroy(mem->box) from
 *     mem_destructor.  The panel framework destroys p->pwid (and thus
 *     mem->box) automatically.
 *
 * Note: mem_usage() shares its name with a similarly-named function in
 *   mem2.c, but both are compiled into separate shared libraries so there
 *   is no link-time conflict in practice.
 */

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>


#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"


/*
 * mem_priv -- private state for one mem plugin instance.
 *
 * plugin    - embedded plugin_instance (MUST be first).
 * mem_pb    - progress bar for RAM usage.
 * swap_pb   - progress bar for swap usage (only valid if show_swap != 0).
 * box       - container box holding mem_pb [and swap_pb].
 * timer     - GLib timeout source ID.
 * show_swap - non-zero if the swap bar is visible.
 */
typedef struct
{
    plugin_instance plugin;
    GtkWidget *mem_pb;
    GtkWidget *swap_pb;
    GtkWidget *box;
    int timer;
    int show_swap;
} mem_priv;

/*
 * mem_type_t -- one /proc/meminfo field descriptor.
 *
 * name  - field name string (e.g. "MemTotal").
 * val   - last parsed value in kB.
 * valid - 1 if val has been set for the current parse pass.
 */
typedef struct
{
    char *name;
    gulong val;
    int valid;
} mem_type_t;

/* Aggregate memory statistics computed from /proc/meminfo. */
typedef struct
{
    struct
    {
        gulong total;
        gulong used;
    } mem;
    struct
    {
        gulong total;
        gulong used;
    } swap;
} stats_t;

/* File-scope stats (shared between mem_usage and mem_update) */
static stats_t stats;

#if defined __linux__
/* First X-macro pass: generate MT_MemTotal, MT_MemFree, … MT_NUM enum */
#undef MT_ADD
#define MT_ADD(x) MT_ ## x,
enum {
#include "mt.h"
    MT_NUM   /* sentinel; equals the total number of tracked fields */
};

/* Second X-macro pass: generate mt[] array with name strings */
#undef MT_ADD
#define MT_ADD(x) { #x, 0, 0 },
mem_type_t mt[] =
{
#include "mt.h"
};

/*
 * mt_match -- try to parse @buf as the /proc/meminfo line for @m.
 *
 * Checks if @buf starts with m->name, then reads the numeric value (kB)
 * that follows the field name and colon.  If successful, sets m->val and
 * m->valid = 1.
 *
 * Parameters:
 *   buf - one line from /proc/meminfo.
 *   m   - the mem_type_t entry to try to match.
 *
 * Returns: TRUE if matched and parsed, FALSE otherwise.
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
 * mem_usage -- read /proc/meminfo and compute stats.mem and stats.swap.
 *
 * Resets all mt[] entries, reads /proc/meminfo line by line, and tries
 * each mt[] entry in turn.  Computes:
 *   mem.used  = MemTotal - (MemFree + Buffers + Cached + Slab)
 *   swap.used = SwapTotal - SwapFree
 *
 * All values are in kB (as reported by /proc/meminfo).
 */
static void
mem_usage()
{
    FILE *fp;
    char buf[160];
    int i;

    fp = fopen("/proc/meminfo", "r");
    if (!fp)
        return;
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
                break;   /* found; skip remaining fields for this line */
        }
    }
    fclose(fp);

    /* Compute stats from parsed values */
    stats.mem.total = mt[MT_MemTotal].val;
    stats.mem.used  = mt[MT_MemTotal].val - (mt[MT_MemFree].val +
        mt[MT_Buffers].val + mt[MT_Cached].val + mt[MT_Slab].val);
    stats.swap.total = mt[MT_SwapTotal].val;
    stats.swap.used  = mt[MT_SwapTotal].val - mt[MT_SwapFree].val;
}
#else
/* Non-Linux stub — no memory information available */
static void
mem_usage()
{
    /* nothing to do on unsupported platforms */
}
#endif

/*
 * mem_update -- GLib timer callback; refresh memory display.
 *
 * Calls mem_usage() to update stats, computes fractional usage [0..1],
 * formats the tooltip (in MB), and updates the progress bar fractions.
 *
 * Parameters:
 *   mem - mem_priv instance.
 *
 * Returns: TRUE (keep the timer running).
 */
static gboolean
mem_update(mem_priv *mem)
{
    gdouble mu, su;
    char str[90];

    ENTER;
    mu = su = 0;
    bzero(&stats, sizeof(stats));
    mem_usage();
    /* compute fractional usage; guard against division by zero */
    if (stats.mem.total)
        mu = (gdouble) stats.mem.used / (gdouble) stats.mem.total;
    if (stats.swap.total)
        su = (gdouble) stats.swap.used / (gdouble) stats.swap.total;
    /* val >> 10 converts kB to MB */
    g_snprintf(str, sizeof(str),
        "<b>Mem:</b> %d%%, %lu MB of %lu MB\n"
        "<b>Swap:</b> %d%%, %lu MB of %lu MB",
        (int)(mu * 100), stats.mem.used >> 10, stats.mem.total >> 10,
        (int)(su * 100), stats.swap.used >> 10, stats.swap.total >> 10);
    DBG("%s\n", str);
    gtk_widget_set_tooltip_markup(mem->plugin.pwid, str);
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR(mem->mem_pb), mu);
    if (mem->show_swap)
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR(mem->swap_pb), su);
    RET(TRUE);
}


/*
 * mem_destructor -- clean up mem plugin resources.
 *
 * Removes the polling timer.  mem->box is a child of p->pwid and will
 * be destroyed by the framework; no explicit gtk_widget_destroy needed.
 */
static void
mem_destructor(plugin_instance *p)
{
    mem_priv *mem = (mem_priv *)p;

    ENTER;
    if (mem->timer)
        g_source_remove(mem->timer);
    RET();
}

/*
 * mem_constructor -- initialise the memory plugin.
 *
 * Reads ShowSwap config, creates the container box and progress bar(s),
 * starts the 3000 ms refresh timer.
 *
 * Orientation:
 *   Horizontal panel: bars grow bottom-to-top, fixed width 9px.
 *   Vertical panel:   bars grow left-to-right, fixed height 9px.
 *
 * Parameters:
 *   p - plugin_instance allocated by the panel framework.
 *
 * Returns: 1 (always succeeds).
 */
static int
mem_constructor(plugin_instance *p)
{
    mem_priv *mem;
    gint w, h;
    GtkProgressBarOrientation o;

    ENTER;
    mem = (mem_priv *) p;
    XCG(p->xc, "ShowSwap", &mem->show_swap, enum, bool_enum);

    /* use panel's orientation-aware box constructor */
    mem->box = p->panel->my_box_new(FALSE, 0);
    gtk_container_set_border_width (GTK_CONTAINER (mem->box), 0);

    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL)
    {
        o = GTK_PROGRESS_BOTTOM_TO_TOP;
        w = 9;   /* fixed narrow width for vertical bar */
        h = 0;   /* stretch to fill height */
    }
    else
    {
        o = GTK_PROGRESS_LEFT_TO_RIGHT;
        w = 0;   /* stretch to fill width */
        h = 9;   /* fixed narrow height for horizontal bar */
    }
    mem->mem_pb = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(mem->box), mem->mem_pb, FALSE, FALSE, 0);
    gtk_progress_bar_set_orientation(GTK_PROGRESS_BAR(mem->mem_pb), o);
    gtk_widget_set_size_request(mem->mem_pb, w, h);

    if (mem->show_swap)
    {
        mem->swap_pb = gtk_progress_bar_new();
        gtk_box_pack_start(GTK_BOX(mem->box), mem->swap_pb, FALSE, FALSE, 0);
        gtk_progress_bar_set_orientation(GTK_PROGRESS_BAR(mem->swap_pb), o);
        gtk_widget_set_size_request(mem->swap_pb, w, h);
    }

    gtk_widget_show_all(mem->box);
    gtk_container_add(GTK_CONTAINER(p->pwid), mem->box);
    gtk_widget_set_tooltip_markup(mem->plugin.pwid, "XXX");   /* placeholder, overwritten by mem_update */
    mem_update(mem);   /* initial reading */
    mem->timer = g_timeout_add(3000, (GSourceFunc) mem_update, (gpointer)mem);
    RET(1);
}

static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "mem",
    .name        = "Memory Monitor",
    .version     = "1.0",
    .description = "Show memory usage",
    .priv_size   = sizeof(mem_priv),

    .constructor = mem_constructor,
    .destructor  = mem_destructor,
};
/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
