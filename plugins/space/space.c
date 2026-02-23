/*
 * space.c -- fbpanel spacer plugin.
 *
 * Provides an invisible fixed-size or expanding spacer between plugins.
 * This is used to push plugins to one side of the panel (e.g., put
 * a clock at the right edge by placing a space plugin before it with
 * expand=true in the panel box).
 *
 * Behaviour:
 *   - Reads a "size" config key (default: 1 pixel).
 *   - For a horizontal panel: sets widget width = size, height = 2.
 *   - For a vertical panel:   sets widget width = 2, height = size.
 *   - When used with expand=true in the plugin block, it grows to fill
 *     all remaining space in the box.
 *
 * Config keys:
 *   size = N   Spacer size in pixels (default: 1).
 *              Set to 0 with expand=true for a pure flexible spacer.
 *
 * No signals, no timers, no private data beyond base plugin_instance.
 */

#include <stdlib.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"


/*
 * space_priv -- private data for one space plugin instance.
 *
 * Fields:
 *   plugin - base plugin_instance (MUST be first; required by plugin.h).
 *   size   - spacer size in pixels from config (read in constructor).
 *   mainw  - reserved; currently unused (always NULL).
 *
 * NOTE: mainw is declared but never assigned; it is dead code.
 */
typedef struct {
    plugin_instance plugin;  /* must be first */
    int size;                /* configured size in pixels */
    GtkWidget *mainw;        /* unused; always NULL */
} space_priv;

/*
 * space_destructor -- clean up the space plugin.
 *
 * Nothing to release: no timers, signals, or allocations.
 * pwid is destroyed by the panel after this returns.
 */
static void
space_destructor(plugin_instance *p)
{
    ENTER;
    RET();
}

/*
 * space_constructor -- initialise the space plugin.
 *
 * Reads the "size" config key (default 1 pixel), then calls
 * gtk_widget_set_size_request() on p->pwid to set the minimum size.
 * The perpendicular dimension is set to 2px (minimal, not zero, to ensure
 * the widget participates in layout).
 *
 * Parameters:
 *   p - the plugin_instance (p->pwid already exists).
 *
 * Returns: 1 (always succeeds).
 */
static int
space_constructor(plugin_instance *p)
{
    int w, h, size;

    ENTER;
    size = 1;                                        /* default: 1px */
    XCG(p->xc, "size", &size, int);                 /* read from config; unchanged if absent */

    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        h = 2;      /* minimal height; panel's height constraint controls actual height */
        w = size;   /* configured width */
    } else {
        w = 2;      /* minimal width */
        h = size;   /* configured height */
    }
    gtk_widget_set_size_request(p->pwid, w, h);  /* request minimum size from GTK */
    RET(1);
}

/* plugin_class descriptor for the space plugin */
static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "space",
    .name        = "Space",
    .version     = "1.0",
    .description = "Ocupy space in a panel",   /* NOTE: typo "Ocupy" in original */
    .priv_size   = sizeof(space_priv),

    .constructor = space_constructor,
    .destructor  = space_destructor,
};
/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
