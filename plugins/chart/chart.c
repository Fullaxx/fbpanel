/*
 * chart.c -- fbpanel scrolling bar-chart helper plugin.
 *
 * Implements a reusable scrolling bar-chart widget used by the cpu, mem2,
 * net, and battery plugins.  chart_priv (defined in chart.h) must be the
 * FIRST member of the consuming plugin's private struct so that the plugin
 * instance pointer can be cast to chart_priv* safely.
 *
 * The chart draws vertical bars left-to-right, scrolling over time.
 * Each column represents one time tick; each tick stores one value per row
 * (row = data series, e.g. "used" vs "idle").
 *
 * Rendering:
 *   - One GdkGC per row, coloured from the colors[] array.
 *   - Bars are drawn from bottom to top; multiple rows stack.
 *   - An etched-in shadow frame is drawn over the bars.
 *   - Background is cleared on each expose (via gdk_window_clear).
 *
 * Public chart_class API (exported via chart_class struct in chart.h):
 *   add_tick(c, val[]) — add one column of float[0..1] values (one per row).
 *   set_rows(c, n, colors[]) — configure number of rows and their colors.
 *
 * Memory:
 *   c->ticks    — a rows × w matrix of gint (pixel heights); reallocated on resize.
 *   c->gc_cpu   — array of GdkGC* (one per row); reallocated when rows change.
 *   Both are freed in chart_destructor via chart_free_ticks / chart_free_gcs.
 *
 * Signals connected in chart_constructor:
 *   "size-allocate" on pwid → chart_size_allocate (reallocates ticks on resize).
 *   "expose-event"  on pwid → chart_expose_event  (clears + redraws).
 *
 * Note: c->da (drawing area) is the same as p->pwid — the GtkBgbox itself.
 */

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <stdlib.h>

#include "plugin.h"
#include "panel.h"
#include "gtkbgbox.h"
#include "chart.h"


//#define DEBUGPRN
#include "dbg.h"


/* Forward declarations — all functions are file-scope static */
static void chart_add_tick(chart_priv *c, float *val);
static void chart_draw(chart_priv *c);
static void chart_size_allocate(GtkWidget *widget, GtkAllocation *a, chart_priv *c);
static gint chart_expose_event(GtkWidget *widget, GdkEventExpose *event, chart_priv *c);

static void chart_alloc_ticks(chart_priv *c);
static void chart_free_ticks(chart_priv *c);
static void chart_alloc_gcs(chart_priv *c, gchar *colors[]);
static void chart_free_gcs(chart_priv *c);

/*
 * chart_add_tick -- add one time-step column of values to the chart.
 *
 * Stores val[i] (clamped to [0..1]) scaled to pixel height c->h into
 * c->ticks[i][c->pos].  Advances c->pos (ring-buffer index) and queues
 * a redraw.
 *
 * Parameters:
 *   c   - chart instance.
 *   val - array of c->rows floats, each in [0..1] representing the fill fraction.
 *
 * Memory: writes to c->ticks (already allocated by chart_alloc_ticks).
 */
static void
chart_add_tick(chart_priv *c, float *val)
{
    int i;

    ENTER;
    if (!c->ticks)
        RET();   /* ticks not yet allocated (widget not yet sized) */
    for (i = 0; i < c->rows; i++) {
        if (val[i] < 0)
            val[i] = 0;    /* clamp to [0..1] */
        if (val[i] > 1)
            val[i] = 1;
        c->ticks[i][c->pos] = val[i] * c->h;   /* convert fraction to pixel height */
        DBG("new wval = %uld\n", c->ticks[i][c->pos]);
    }
    c->pos = (c->pos + 1) %  c->w;   /* advance ring-buffer position (wraps) */
    gtk_widget_queue_draw(c->da);     /* schedule redraw */

    RET();
}

/*
 * chart_draw -- render all columns into the chart's GDK window.
 *
 * Draws vertical stacked bars for each column (skip col 0 and col c->w-1
 * as borders).  For each column, draws rows bottom-to-top, each row's bar
 * in its configured colour.
 *
 * Parameters:
 *   c - chart instance (uses c->ticks, c->gc_cpu, c->w, c->h, c->rows, c->pos).
 *
 * Note: Column index is computed as (i + c->pos) % c->w to implement the
 *   scrolling ring-buffer effect.
 */
static void
chart_draw(chart_priv *c)
{
    int j, i, y;

    ENTER;
    if (!c->ticks)
        RET();
    for (i = 1; i < c->w-1; i++) {
        y = c->h-2;  /* start drawing from near the bottom */
        for (j = 0; j < c->rows; j++) {
            int val;

            /* ring-buffer: oldest column is at pos, newest is at pos-1 */
            val = c->ticks[j][(i + c->pos) % c->w];
            if (val)
                gdk_draw_line(c->da->window, c->gc_cpu[j], i, y, i, y - val);
            y -= val;   /* stack rows upward */
        }
    }
    RET();
}

/*
 * chart_size_allocate -- "size-allocate" handler: resize the tick buffer.
 *
 * Called when pwid is allocated new dimensions.  Frees and reallocates the
 * tick matrix (rows × w) to match the new pixel size.  Also updates the
 * frame area and fill area extents used by chart_expose_event.
 *
 * Parameters:
 *   widget - the pwid widget (not used directly; c->da is the same).
 *   a      - the new allocation (a->width, a->height).
 *   c      - chart instance.
 *
 * Memory: chart_free_ticks + chart_alloc_ticks on every size change.
 */
static void
chart_size_allocate(GtkWidget *widget, GtkAllocation *a, chart_priv *c)
{
    ENTER;
    if (c->w != a->width || c->h != a->height) {
        chart_free_ticks(c);
        c->w = a->width;
        c->h = a->height;
        chart_alloc_ticks(c);
        /* full drawing area */
        c->area.x = 0;
        c->area.y = 0;
        c->area.width = a->width;
        c->area.height = a->height;
        /* filled area for the etched shadow frame */
        if (c->plugin.panel->transparent) {
            /* transparent background: fill entire area */
            c->fx = 0;
            c->fy = 0;
            c->fw = a->width;
            c->fh = a->height;
        } else if (c->plugin.panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
            /* horizontal panel: leave 1px top/bottom border */
            c->fx = 0;
            c->fy = 1;
            c->fw = a->width;
            c->fh = a->height -2;
        } else {
            /* vertical panel: leave 1px left/right border */
            c->fx = 1;
            c->fy = 0;
            c->fw = a->width -2;
            c->fh = a->height;
        }
    }
    gtk_widget_queue_draw(c->da);
    RET();
}


/*
 * chart_expose_event -- "expose-event" handler: clear and redraw chart.
 *
 * Called by GTK when the widget needs repainting.  Clears the window
 * background (reveals the panel pseudo-transparency), draws the bars,
 * then draws an etched-in shadow frame around the fill area.
 *
 * Parameters:
 *   widget - the drawing area widget (c->da).
 *   event  - expose event (clipping rect — not used; we redraw all).
 *   c      - chart instance.
 *
 * Returns: FALSE (allow further signal handlers to run).
 */
static gint
chart_expose_event(GtkWidget *widget, GdkEventExpose *event, chart_priv *c)
{
    ENTER;
    gdk_window_clear(widget->window);  /* clear to background (pseudo-transparency) */
    chart_draw(c);                     /* draw bars */

    /* draw etched-in shadow frame over the chart */
    gtk_paint_shadow(widget->style, widget->window,
        widget->state, GTK_SHADOW_ETCHED_IN,
        &c->area, widget, "frame", c->fx, c->fy, c->fw, c->fh);

    RET(FALSE);
}

/*
 * chart_alloc_ticks -- allocate the ring-buffer tick matrix.
 *
 * Allocates a rows × w matrix of gint (pixel heights), initialised to 0.
 * Sets c->pos = 0 (next write position).
 *
 * Parameters:
 *   c - chart instance (uses c->rows, c->w).
 *
 * Memory: c->ticks is g_new0'd; must be freed with chart_free_ticks.
 *   Each row array is also g_new0'd.
 */
static void
chart_alloc_ticks(chart_priv *c)
{
    int i;

    ENTER;
    if (!c->w || !c->rows)
        RET();   /* nothing to allocate yet */
    c->ticks = g_new0(gint *, c->rows);
    for (i = 0; i < c->rows; i++) {
        c->ticks[i] = g_new0(gint, c->w);
        if (!c->ticks[i])
            DBG2("can't alloc mem: %p %d\n", c->ticks[i], c->w);
    }
    c->pos = 0;   /* start ring-buffer at position 0 */
    RET();
}


/*
 * chart_free_ticks -- free the ring-buffer tick matrix.
 *
 * Frees each row array, then the array of row pointers.
 * Sets c->ticks = NULL.
 *
 * Parameters:
 *   c - chart instance.
 */
static void
chart_free_ticks(chart_priv *c)
{
    int i;

    ENTER;
    if (!c->ticks)
        RET();
    for (i = 0; i < c->rows; i++)
        g_free(c->ticks[i]);   /* free each row */
    g_free(c->ticks);          /* free the row pointer array */
    c->ticks = NULL;
    RET();
}


/*
 * chart_alloc_gcs -- allocate GdkGC array and set colours.
 *
 * Allocates c->gc_cpu (array of c->rows GdkGC*) and initialises each with
 * the parsed colour from colors[i].  Each GC is created on the panel window.
 *
 * Parameters:
 *   c      - chart instance.
 *   colors - array of c->rows colour name strings (e.g. "green", "#ff0000").
 *
 * Memory: c->gc_cpu is g_new0'd.  Each GC is freed in chart_free_gcs with
 *   g_object_unref().
 */
static void
chart_alloc_gcs(chart_priv *c, gchar *colors[])
{
    int i;
    GdkColor color;

    ENTER;
    c->gc_cpu = g_new0( typeof(*c->gc_cpu), c->rows);
    if (c->gc_cpu) {
        for (i = 0; i < c->rows; i++) {
            /* create a GC on the panel's top window */
            c->gc_cpu[i] = gdk_gc_new(c->plugin.panel->topgwin->window);
            gdk_color_parse(colors[i], &color);
            gdk_colormap_alloc_color(
                gdk_drawable_get_colormap(c->plugin.panel->topgwin->window),
                &color, FALSE, TRUE);
            gdk_gc_set_foreground(c->gc_cpu[i],  &color);
        }
    }
    RET();
}


/*
 * chart_free_gcs -- free the GdkGC array.
 *
 * Unrefs each GdkGC, then frees the array.  Sets c->gc_cpu = NULL.
 *
 * Parameters:
 *   c - chart instance.
 */
static void
chart_free_gcs(chart_priv *c)
{
    int i;

    ENTER;
    if (c->gc_cpu) {
        for (i = 0; i < c->rows; i++)
            g_object_unref(c->gc_cpu[i]);   /* release GdkGC reference */
        g_free(c->gc_cpu);
        c->gc_cpu = NULL;
    }
    RET();
}


/*
 * chart_set_rows -- reconfigure the number of data rows and their colours.
 *
 * Frees the old ticks and GCs, updates c->rows, and reallocates both.
 * Must be called by the consuming plugin's constructor before add_tick.
 *
 * Parameters:
 *   c      - chart instance.
 *   num    - number of rows (must be 1..9; asserted).
 *   colors - array of num colour name strings.
 */
static void
chart_set_rows(chart_priv *c, int num, gchar *colors[])
{
    ENTER;
    g_assert(num > 0 && num < 10);  /* sanity: 1..9 rows only */
    chart_free_ticks(c);
    chart_free_gcs(c);
    c->rows = num;
    chart_alloc_ticks(c);
    chart_alloc_gcs(c, colors);
    RET();
}

/*
 * chart_constructor -- initialise the chart plugin.
 *
 * Connects size-allocate and expose-event signals to the pwid widget.
 * Sets c->da = pwid (the chart draws directly on the plugin's GtkBgbox).
 * Sets a minimum size request of 40×25 pixels.
 *
 * Note: The consuming plugin (e.g., cpu, mem2) must call k->set_rows()
 *   after chart_constructor to configure the number of rows and colours.
 *
 * Parameters:
 *   p - plugin_instance (must be cast to chart_priv* by the caller's struct layout).
 *
 * Returns: 1 (always succeeds).
 */
static int
chart_constructor(plugin_instance *p)
{
    chart_priv *c;

    ENTER;
    /* must be allocated by caller (chart_priv is embedded as first member) */
    c = (chart_priv *) p;
    c->rows = 0;
    c->ticks = NULL;
    c->gc_cpu = NULL;
    c->da = p->pwid;   /* draw directly on the plugin widget */

    gtk_widget_set_size_request(c->da, 40, 25);   /* minimum 40×25 px */
    g_signal_connect (G_OBJECT (p->pwid), "size-allocate",
          G_CALLBACK (chart_size_allocate), (gpointer) c);

    g_signal_connect_after (G_OBJECT (p->pwid), "expose-event",
          G_CALLBACK (chart_expose_event), (gpointer) c);

    RET(1);
}

/*
 * chart_destructor -- clean up chart resources.
 *
 * Frees the tick matrix and GdkGC array.
 * The pwid widget is destroyed by the panel framework after this returns.
 *
 * Parameters:
 *   p - plugin_instance.
 */
static void
chart_destructor(plugin_instance *p)
{
    chart_priv *c = (chart_priv *) p;

    ENTER;
    chart_free_ticks(c);
    chart_free_gcs(c);
    RET();
}

/*
 * chart_class descriptor.
 *
 * The chart plugin exposes two extra methods beyond the standard plugin_class:
 *   add_tick — add one time-step of data.
 *   set_rows — configure row count and colours.
 *
 * Consuming plugins use class_get("chart") to obtain this descriptor and
 * call these methods via the returned chart_class*.
 */
static chart_class class = {
    .plugin = {
        .type        = "chart",
        .name        = "Chart",
        .description = "Basic chart plugin",
        .priv_size   = sizeof(chart_priv),

        .constructor = chart_constructor,
        .destructor  = chart_destructor,
    },
    .add_tick = chart_add_tick,
    .set_rows = chart_set_rows,
};
/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
