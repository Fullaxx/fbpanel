/*
 * chart.h -- public interface for the fbpanel "chart" plugin base class.
 *
 * The chart plugin provides a scrolling strip-chart widget backed by
 * GDK drawing primitives (gdk_draw_line).  Other plugins (cpu, mem2, net)
 * use chart as a base class by embedding chart_priv as their FIRST struct
 * member and obtaining the chart_class vtable with class_get("chart").
 *
 * Subclass usage pattern:
 *   1. Call class_get("chart") to get a chart_class*.
 *   2. Call PLUGIN_CLASS(k)->constructor(p) to initialise the chart widget.
 *   3. Call k->set_rows() to configure the number of data rows and their colours.
 *   4. Call k->add_tick() on each polling cycle to push new data points.
 *   5. Call PLUGIN_CLASS(k)->destructor(p) and class_put("chart") when done.
 */

#ifndef CHART_H
#define CHART_H


#include "plugin.h"
#include "panel.h"


/*
 * chart_priv -- per-instance private data for the chart base class.
 *
 * Layout requirement: plugin_instance MUST be the first field of
 * plugin_instance so that chart_priv* can be safely cast to
 * plugin_instance* by the fbpanel framework.
 *
 * Subclasses (cpu_priv, mem2_priv, net_priv) embed chart_priv as their
 * FIRST field, which means chart_priv* and subclass* are interchangeable.
 *
 * Memory ownership:
 *   gc_cpu  -- array of c->rows GdkGC* objects; allocated by chart_alloc_gcs(),
 *              freed by chart_free_gcs().  Each GdkGC* is ref-counted;
 *              freed via g_object_unref().
 *   ticks   -- 2-D array [rows][width]; allocated by chart_alloc_ticks(),
 *              freed by chart_free_ticks().  Both the outer pointer array
 *              and each inner row array are heap-allocated with g_new0().
 *   da      -- alias for p->pwid (the GtkDrawingArea); NOT owned by chart_priv;
 *              lifetime managed by the GTK widget hierarchy.
 *
 * Signal management:
 *   chart_constructor() connects two signals on p->pwid:
 *     "size-allocate"  -> chart_size_allocate  (with g_signal_connect)
 *     "expose-event"   -> chart_expose_event   (with g_signal_connect_after)
 *   These signal handlers are NOT explicitly disconnected in
 *   chart_destructor() -- they will be disconnected automatically when
 *   the widget is destroyed by the GTK framework.  However, if the widget
 *   outlives the chart_priv data, stale callbacks could fire.
 *   See BUGS section.
 */
typedef struct {
    plugin_instance plugin; // MUST be first: superclass (framework casts to this)

    GdkGC **gc_cpu;         // array of c->rows GDK graphics contexts, one per data row
                            // NULL until chart_alloc_gcs() is called; freed by chart_free_gcs()

    GtkWidget *da;          // the GtkDrawingArea widget (alias of plugin->pwid); NOT owned here

    gint **ticks;           // 2-D scrolling data buffer: ticks[row][column]
                            // each entry is a pre-scaled pixel height (0..c->h)
                            // NULL until chart_alloc_ticks() is called

    gint pos;               // write position (column index) in the ticks ring buffer;
                            // advances modulo c->w on each add_tick() call

    gint w, h;              // current widget width and height in pixels
                            // set by chart_size_allocate(); used to scale tick values

    gint rows;              // number of data rows (set by set_rows()); 0 until initialised

    GdkRectangle area;      // full widget rectangle used for the GTK shadow frame
                            // {0, 0, width, height}

    /* Shadow frame coordinates -- adjusted for panel orientation so the
     * frame border does not overlap the panel edge:
     *   fx, fy -- top-left corner of the frame border
     *   fw, fh -- width and height of the frame border */
    int fx, fy, fw, fh;
} chart_priv;

/*
 * chart_class -- vtable for the chart plugin base class.
 *
 * Extends plugin_class with two additional virtual methods:
 *   add_tick  -- push a new set of per-row float values (0.0..1.0) into
 *                the ring buffer and schedule a redraw.
 *   set_rows  -- (re)configure the number of data rows, resetting the
 *                tick buffer and recreating the GDK graphics contexts.
 *
 * Subclasses obtain a pointer to this struct via class_get("chart").
 * They must call class_put("chart") in their destructor.
 */
typedef struct {
    plugin_class plugin; // MUST be first: base class vtable (constructor/destructor/etc.)

    /*
     * add_tick -- append a new sample to the strip chart.
     *
     * Parameters:
     *   c   -- chart_priv* for the plugin instance.
     *   val -- array of c->rows float values, each in [0.0, 1.0].
     *          Values are clamped to [0, 1] before storage.
     *          val[i] represents the fractional utilisation for row i
     *          (e.g. 0.75 = 75% CPU usage).
     *
     * Side effects: Advances c->pos, schedules a GTK redraw via
     *   gtk_widget_queue_draw().
     */
    void (*add_tick)(chart_priv *c, float *val);

    /*
     * set_rows -- configure the number of data series for the chart.
     *
     * Frees existing tick buffers and GDK graphics contexts, then
     * re-allocates them for `num` rows with the provided colours.
     *
     * Parameters:
     *   c      -- chart_priv* for the plugin instance.
     *   num    -- number of data rows; must satisfy 0 < num < 10
     *             (enforced with g_assert; values outside this range abort).
     *   colors -- NULL-terminated array of num colour name strings
     *             (e.g. {"green", "blue", NULL}).  Each string is parsed
     *             with gdk_color_parse().  Must not be NULL.
     *
     * Note: This function resets c->pos to 0 (via chart_alloc_ticks).
     */
    void (*set_rows)(chart_priv *c, int num, gchar *colors[]);
} chart_class;


#endif /* CHART_H */
