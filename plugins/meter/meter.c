/*
 * plugins/meter/meter.c -- Implementation of the fbpanel "meter" plugin.
 *
 * PURPOSE
 * -------
 * Provides a reusable icon-based level-indicator widget.  The plugin itself
 * does not display any useful data directly; instead, client plugins (such as
 * a battery or volume indicator) obtain a pointer to meter_class, call its
 * constructor to create the widget, register a set of icon names via
 * set_icons(), and periodically call set_level(0..100) to update the display.
 *
 * ICON SELECTION
 * --------------
 * The level [0, 100] is linearly mapped to an index in the icon array:
 *   index = round(level / 100.0 * (num_icons - 1))
 * where round() uses roundf() from <math.h> (declared but not #included here;
 * the declaration is provided inline -- see BUG below).
 *
 * ICON THEME CHANGE HANDLING
 * --------------------------
 * When the GTK icon theme changes (global icon_theme "changed" signal), the
 * update_view() function forces a reload of the current icon by resetting
 * cur_icon to -1 and calling meter_set_level() with the existing level.
 *
 * PUBLIC API (through meter_class vtable)
 * ----------------------------------------
 *   set_level(meter_priv *m, int level)  -- update displayed level [0..100]
 *   set_icons(meter_priv *m, gchar **icons) -- register icon array
 *
 * The standard plugin_class.constructor and plugin_class.destructor are also
 * exported via the class vtable.
 */

#include "plugin.h"
#include "panel.h"
#include "meter.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * roundf -- forward declaration of the C99 math.h function.
 *
 * BUG: roundf() should be brought in via #include <math.h> rather than a
 *      bare forward declaration.  Without the header, the compiler may assume
 *      an incorrect ABI (e.g. returning int on some platforms) and the link
 *      may require -lm.  The declaration here is fragile and non-portable.
 */
float roundf(float x);

/*
 * meter_set_level -- update the meter's displayed level.
 *
 * Maps @level (0..100) to an index into m->icons[] using rounding, then
 * loads and displays the corresponding icon from the GTK icon theme.
 * If the computed icon index equals the currently displayed one, the
 * function returns early without touching the GtkImage.
 *
 * Parameters:
 *   m     -- the meter_priv instance to update.
 *   level -- integer percentage in the range [0, 100].
 *
 * Returns: void.
 *
 * Side effects:
 *   - Updates m->cur_icon and m->level.
 *   - Calls gtk_image_set_from_pixbuf() to refresh the widget.
 *   - Loads an icon from icon_theme; if loading fails, the image is set to
 *     NULL (blank / empty image).
 *
 * Memory notes:
 *   - The GdkPixbuf `pb` returned by gtk_icon_theme_load_icon() is owned by
 *     the caller; after being set on the GtkImage (which takes a reference),
 *     pb is unreferenced here.  If pb is NULL (icon load failure), the
 *     gtk_image_set_from_pixbuf(NULL) call clears the image.
 *
 * BUG: m->level is declared as gfloat but @level is an int.  The no-change
 *      check "m->level == level" compares a float to an int via implicit
 *      conversion.  If level was previously stored after a float operation
 *      (e.g. -1.0), the comparison may miss cases where the int level happens
 *      to equal the truncated float value.  The type should be unified (use
 *      gint for m->level or cast explicitly).
 *
 * BUG: If m->icons is NULL (set_icons was never called) but m->num is
 *      non-zero (should not happen but defensively), the m->icons[i] access
 *      below would be a NULL-pointer dereference.  The m->num guard prevents
 *      this as long as invariants hold.
 */
static void
meter_set_level(meter_priv *m, int level)
{
    int i;
    GdkPixbuf *pb;

    ENTER;
    /* Early exit if the level is unchanged. */
    if (m->level == level)
        RET();
    /* Early exit if no icons have been registered. */
    if (!m->num)
        RET();
    /* Validate range. */
    if (level < 0 || level > 100) {
        ERR("meter: illegal level %d\n", level);
        RET();
    }

    /* Map level percentage to icon array index using rounding. */
    i = roundf((gfloat) level / 100 * (m->num - 1));
    DBG("level=%f icon=%d\n", level, i);

    if (i != m->cur_icon) {
        m->cur_icon = i;
        /* Load the icon at the required size from the current theme. */
        pb = gtk_icon_theme_load_icon(icon_theme, m->icons[i],
            m->size, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
        DBG("loading icon '%s' %s\n", m->icons[i], pb ? "ok" : "failed");
        /* Update the GtkImage; passing NULL clears the image. */
        gtk_image_set_from_pixbuf(GTK_IMAGE(m->meter), pb);
        if (pb)
            g_object_unref(G_OBJECT(pb)); /* release our reference */
    }
    m->level = level; /* record new level (stored as gfloat from int) */
    RET();
}

/*
 * meter_set_icons -- register the icon-name array for level display.
 *
 * Records the pointer to @icons and counts the elements.  Resets the
 * current-icon tracking state to force a full redraw on the next
 * set_level() call regardless of the current level.
 *
 * Parameters:
 *   m     -- the meter_priv instance to configure.
 *   icons -- NULL-terminated array of GTK icon-theme name strings.
 *            Ownership remains with the caller; meter_priv only holds a
 *            non-owning pointer.  The array must stay valid until
 *            set_icons() is called again or the meter is destroyed.
 *
 * Returns: void.
 *
 * Side effects:
 *   Sets m->num, m->icons, m->cur_icon (-1), m->level (-1).
 *
 * Memory note:
 *   The old m->icons array is NOT freed here (it is not owned by meter_priv).
 *   If the client changes icon sets it is responsible for freeing the old array
 *   after calling set_icons() with the new one.
 *
 * BUG: The identity check "m->icons == icons" is a pointer comparison, not
 *      a content comparison.  If a caller rebuilds the icon array at a new
 *      address with the same strings, the check will NOT short-circuit and the
 *      state will be needlessly reset.  This is a minor efficiency issue, not
 *      a correctness bug.
 */
static void
meter_set_icons(meter_priv *m, gchar **icons)
{
    gchar **s;

    ENTER;
    /* No-op if the exact same array pointer is already registered. */
    if (m->icons == icons)
        RET();

    /* Count elements and log them at debug level. */
    for (s = icons; *s; s++)
        DBG("icon %s\n", *s);
    m->num = (s - icons); /* number of non-NULL entries */
    DBG("total %d icons\n", m->num);

    m->icons = icons;    /* store non-owning pointer */
    m->cur_icon = -1;    /* force icon reload on next set_level() */
    m->level = -1;       /* mark level as unset so any value triggers redraw */
    RET();
}

/*
 * update_view -- force a redisplay of the current icon after a theme change.
 *
 * Connected to the global icon_theme "changed" signal.  Resets m->cur_icon
 * to -1 so that meter_set_level() will reload the icon from the new theme
 * even if the level value has not changed.
 *
 * Parameters:
 *   m -- the meter_priv instance (passed as gpointer by GLib signal machinery).
 *
 * Side effects:
 *   Calls meter_set_level() which may update the GtkImage widget.
 */
static void
update_view(meter_priv *m)
{
    ENTER;
    m->cur_icon = -1;       /* invalidate cached icon index */
    meter_set_level(m, m->level); /* reload the icon for the current level */
    RET();
}

/*
 * meter_constructor -- plugin_class.constructor for the meter plugin.
 *
 * Creates a GtkImage widget, adds it to the plugin container, and connects
 * the icon-theme change signal.
 *
 * Parameters:
 *   p -- plugin_instance* allocated by the panel framework.
 *
 * Returns:
 *   1 on success (non-zero as required by plugin_class.constructor contract).
 *
 * Side effects:
 *   Sets m->meter, m->cur_icon, m->size, m->itc_id.
 *   Adds the GtkImage as a child of p->pwid.
 *   Connects update_view to icon_theme "changed".
 *
 * Memory notes:
 *   m->meter is owned by the GTK widget hierarchy; do not unref it directly.
 *   m->itc_id is used in meter_destructor() to disconnect the signal.
 */
static int
meter_constructor(plugin_instance *p)
{
    meter_priv *m;

    ENTER;
    m = (meter_priv *) p;

    /* Create an empty GtkImage as the display widget. */
    m->meter = gtk_image_new();
    gtk_misc_set_alignment(GTK_MISC(m->meter), 0.5, 0.5); /* centre the image */
    gtk_misc_set_padding(GTK_MISC(m->meter), 0, 0);
    gtk_widget_show(m->meter);
    gtk_container_add(GTK_CONTAINER(p->pwid), m->meter);

    m->cur_icon = -1; /* no icon displayed yet */
    /* Use panel's icon height as the target icon size. */
    m->size = p->panel->max_elem_height;

    /* Reload the icon whenever the user switches GTK icon themes. */
    m->itc_id = g_signal_connect_swapped(G_OBJECT(icon_theme),
        "changed", (GCallback) update_view, m);
    RET(1);
}

/*
 * meter_destructor -- plugin_class.destructor for the meter plugin.
 *
 * Disconnects the icon-theme change signal.  The GtkImage widget is owned
 * by the parent container and will be destroyed when the container is torn
 * down by the panel framework; no explicit widget destruction is needed here.
 *
 * Parameters:
 *   p -- plugin_instance* being destroyed.
 *
 * Side effects:
 *   Disconnects icon_theme "changed" signal using stored m->itc_id.
 *
 * FIXME: m->icons is not freed here, which is correct only if the client
 *        plugin frees it separately.  There is no documented contract forcing
 *        clients to do so.  If the client forgets, the icon array leaks.
 */
static void
meter_destructor(plugin_instance *p)
{
    meter_priv *m = (meter_priv *) p;

    ENTER;
    /* Disconnect the icon-theme-changed handler to prevent calls after teardown. */
    g_signal_handler_disconnect(G_OBJECT(icon_theme), m->itc_id);
    RET();
}

/*
 * class -- static plugin descriptor for the "meter" plugin.
 *
 * Extends the base plugin_class with set_level and set_icons function
 * pointers, allowing client plugins to drive the meter display.
 */
static meter_class class = {
    .plugin = {
        .type        = "meter",
        .name        = "Meter",
        .description = "Basic meter plugin",
        .version     = "1.0",
        .priv_size   = sizeof(meter_priv), /* bytes allocated per instance */

        .constructor = meter_constructor,
        .destructor  = meter_destructor,
    },
    .set_level = meter_set_level,  /* virtual function: update level display */
    .set_icons = meter_set_icons,  /* virtual function: register icon array */
};

/* class_ptr -- exported symbol used by the panel plugin loader. */
static plugin_class *class_ptr = (plugin_class *) &class;
