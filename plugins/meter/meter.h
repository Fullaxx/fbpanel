/*
 * plugins/meter/meter.h -- Private header for the fbpanel "meter" plugin.
 *
 * PURPOSE
 * -------
 * Defines the data structures for the meter plugin, which provides a
 * generic icon-based level indicator.  A client plugin (e.g. the battery
 * or volume plugin) calls the meter's virtual functions to supply an
 * array of icon names and set the current level (0-100), and the meter
 * selects and displays the corresponding icon.
 *
 * DESIGN PATTERN
 * --------------
 * The meter plugin follows the same "subclassing by embedding" convention
 * used throughout fbpanel:
 *   - meter_priv embeds plugin_instance as its first field, allowing
 *     safe C-style casting between the two types.
 *   - meter_class embeds plugin_class and adds two extra virtual functions
 *     (set_level, set_icons) that client plugins call to drive the widget.
 *
 * A client plugin typically does:
 *   1. Call class_get("meter") to obtain a meter_class*.
 *   2. Call PLUGIN_CLASS(k)->constructor(p) to initialise the meter widget.
 *   3. Call k->set_icons(m, icon_name_array) once to register icons.
 *   4. Periodically call k->set_level(m, 0..100) to update the display.
 *   5. On teardown, call PLUGIN_CLASS(k)->destructor(p) and class_put("meter").
 *
 * PUBLIC API
 * ----------
 *   None exposed to the panel framework directly -- the plugin loader finds
 *   the "meter" type via the class_ptr variable in meter.c.
 *
 * KEY DATA STRUCTURES
 * -------------------
 *   meter_priv  -- per-instance state (icon cache, current level, etc.)
 *   meter_class -- per-type vtable with set_level / set_icons ops
 */

#ifndef meter_H
#define meter_H

#include "plugin.h"   /* plugin_instance, plugin_class */
#include "panel.h"    /* panel struct, icon_theme global */

/*
 * meter_priv -- per-instance private state for the meter plugin.
 *
 * Inherits plugin_instance by embedding it as the FIRST field so that
 * (meter_priv *) and (plugin_instance *) are interchangeable via casting.
 *
 * Ownership notes:
 *   icons    -- pointer to a NULL-terminated array of icon-name strings.
 *               Owned by the CLIENT plugin (not by meter_priv); meter only
 *               holds a non-owning reference.  Must remain valid while the
 *               meter is in use.
 *   meter    -- GtkImage widget owned by the GTK widget hierarchy (child of
 *               plugin->pwid).  Not explicitly destroyed by meter_destructor
 *               (the parent container destroys it).
 *   itc_id   -- GLib signal handler ID connecting icon_theme "changed" to
 *               update_view().  Disconnected in meter_destructor().
 */
typedef struct {
    /* Base class -- MUST be first field (fbpanel casting convention). */
    plugin_instance plugin;

    /* Non-owning pointer to a NULL-terminated array of GTK icon-theme names,
     * one per discrete level step.  Set by meter_set_icons().
     * Example: { "battery-caution", "battery-low", "battery-good", NULL }
     *
     * FIXME: the array is not owned by meter_priv; if the client frees it
     * while the meter is still active, meter_set_level() will access freed
     * memory (use-after-free). */
    gchar **icons;

    /* Number of icons in the `icons` array (computed in meter_set_icons() as
     * the distance from icons[0] to the terminating NULL). */
    gint num;

    /* The GtkImage widget that displays the currently selected icon.
     * Owned by the GTK widget hierarchy; do not g_object_unref() it directly. */
    GtkWidget *meter;

    /* Current level (0.0 to 100.0, or -1.0 when unset/stale).
     * Stored as a float even though the public API takes int; the float
     * is used to detect no-change in meter_set_level(). */
    gfloat level;

    /* Index into the `icons` array that was last loaded into the GtkImage.
     * -1 means no icon has been displayed yet (forces a reload on next
     * meter_set_level() call even if the computed index is 0). */
    gint cur_icon;

    /* Icon size in pixels, taken from panel->max_elem_height at construction.
     * Used in the gtk_icon_theme_load_icon() call inside meter_set_level(). */
    gint size;

    /* GLib signal handler ID for the icon-theme "changed" connection.
     * Stored so meter_destructor() can disconnect it cleanly via
     * g_signal_handler_disconnect(). */
    gint itc_id;
} meter_priv;

/*
 * meter_class -- per-type descriptor/vtable for the meter plugin.
 *
 * Extends plugin_class with two additional virtual functions that client
 * plugins must call to configure and drive the meter display.
 *
 * Fields (beyond plugin_class):
 *   set_level  -- update the displayed level (0..100).
 *   set_icons  -- register the icon-name array and count.
 */
typedef struct {
    /* Base vtable -- MUST be first field (fbpanel casting convention). */
    plugin_class plugin;

    /*
     * set_level -- set the meter's current level and refresh the icon.
     *
     * Parameters:
     *   c   -- the meter_priv instance to update.
     *   val -- integer level in the range [0, 100].
     *
     * Behaviour:
     *   - If val equals the current level, the call is a no-op.
     *   - Maps val to an icon index: round(val/100 * (num-1)).
     *   - Loads the icon from the GTK icon theme at m->size pixels.
     *   - If the icon load fails, the GtkImage is set to NULL (blank).
     *
     * Thread safety: must be called from the GLib main thread.
     */
    void (*set_level)(meter_priv *c, int val);

    /*
     * set_icons -- register the array of icon names for level steps.
     *
     * Parameters:
     *   c     -- the meter_priv instance to configure.
     *   icons -- NULL-terminated array of GTK icon-theme name strings.
     *            The array and strings are NOT copied; the client retains
     *            ownership and must keep them valid until set_icons is called
     *            again or the meter is destroyed.
     *
     * Behaviour:
     *   - Records the pointer and counts the elements.
     *   - Resets cur_icon to -1 and level to -1 to force a full redraw on
     *     the next set_level() call.
     *   - If icons == m->icons (same pointer), the call is a no-op.
     */
    void (*set_icons)(meter_priv *c, gchar **icons);
} meter_class;


#endif /* meter_H */
