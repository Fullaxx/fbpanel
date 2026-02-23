/*
 * separator.c -- fbpanel separator plugin.
 *
 * Draws a single GTK separator widget (GtkHSeparator or GtkVSeparator,
 * depending on panel orientation) to visually divide plugin groups.
 *
 * This is the simplest possible fbpanel plugin:
 *   - No private state beyond the base plugin_instance.
 *   - Constructor: creates one separator and adds it to pwid.
 *   - Destructor: nothing to clean up (child widget destroyed with pwid).
 *
 * The separator widget is provided by the panel's my_separator_new()
 * factory (set in panel_start_gui() based on orientation: GtkHSeparator
 * for horizontal panels, GtkVSeparator for vertical panels).
 *
 * Config block: none (no plugin-specific config keys).
 */

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"


/*
 * separator_constructor -- initialise the separator plugin instance.
 *
 * Parameters:
 *   p - the plugin_instance (p->pwid already created by panel).
 *
 * Creates one GtkHSeparator or GtkVSeparator (via panel->my_separator_new),
 * adds it to p->pwid, and makes the widget visible.
 *
 * Returns: 1 (always succeeds; a separator can always be created).
 *
 * Memory: sep is added to p->pwid; ownership transfers to pwid.
 *         When pwid is destroyed, sep is destroyed automatically.
 */
static int
separator_constructor(plugin_instance *p)
{
    GtkWidget *sep;

    ENTER;
    sep = p->panel->my_separator_new();          /* orientation-appropriate separator */
    gtk_container_add(GTK_CONTAINER(p->pwid), sep);
    gtk_widget_show_all(p->pwid);                /* show sep and pwid */
    RET(1);
}

/*
 * separator_destructor -- clean up the separator plugin instance.
 *
 * Parameters:
 *   p - the plugin_instance being destroyed.
 *
 * Nothing to clean up: no timers, no signal connections, no allocated memory.
 * The sep widget is destroyed automatically when pwid is destroyed by the panel.
 */
static void
separator_destructor(plugin_instance *p)
{
    ENTER;
    RET();
}


/* plugin_class descriptor for the separator plugin */
static plugin_class class = {
    .count       = 0,
    .type        = "separator",      /* config type string */
    .name        = "Separator",      /* display name */
    .version     = "1.0",
    .description = "Separator line",
    .priv_size   = sizeof(plugin_instance),  /* no extra private data */

    .constructor = separator_constructor,
    .destructor  = separator_destructor,
};
/* Required: pointer used by PLUGIN macro for auto-registration on dlopen */
static plugin_class *class_ptr = (plugin_class *) &class;
