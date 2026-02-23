/*
 * plugins/menu/menu.h -- Private header for the fbpanel "menu" plugin.
 *
 * PURPOSE
 * -------
 * Declares the private per-instance state (menu_priv) and the per-type
 * descriptor (menu_class) used by the menu plugin.  Both structs follow the
 * standard fbpanel "subclassing-by-embedding" convention: the panel framework
 * casts plugin_instance* to menu_priv* (and plugin_class* to menu_class*)
 * freely because the embedded base struct is always the FIRST member.
 *
 * PUBLIC API
 * ----------
 * None -- this header is only included by the plugin's own .c files.
 * The plugin is exported to the rest of the panel via the file-scope
 * `class_ptr` variable in menu.c, which the panel's plugin loader discovers
 * by convention.
 *
 * KEY DATA STRUCTURES
 * -------------------
 *   menu_priv  -- per-instance private state
 *   menu_class -- per-type vtable (extends plugin_class with no extra ops)
 */

#ifndef MENU_H
#define MENU_H

#include "plugin.h"   /* plugin_instance, plugin_class */
#include "panel.h"    /* panel struct, ah_start/ah_stop, menu_pos */

/* Default icon size (pixels) for menu item icons when the configuration
 * does not provide an explicit "iconsize" value. */
#define MENU_DEFAULT_ICON_SIZE 22

/*
 * menu_priv -- per-instance private state for the menu plugin.
 *
 * Inherits plugin_instance by embedding it as the FIRST field.  The panel
 * framework allocates sizeof(menu_priv) bytes with g_malloc0() and casts the
 * resulting pointer freely between (plugin_instance *) and (menu_priv *).
 *
 * Lifetime / ownership summary:
 *   menu  -- GtkMenu widget owned by the GTK widget hierarchy.
 *             Created lazily in menu_create(), destroyed in menu_destroy().
 *   bg    -- GtkWidget (toolbar button) owned by GTK, child of plugin->pwid.
 *             Created in make_button(), destroyed in menu_destructor().
 *   xc    -- Expanded xconf tree owned by this struct.
 *             Created in menu_create(), freed (deep=FALSE) in menu_destroy().
 *   tout  -- GLib timer source ID; 0 means no active source.
 *   rtout -- GLib timer source ID; 0 means no active source.
 */
typedef struct {
    /* Base class -- MUST be the first field so that (menu_priv *) and
     * (plugin_instance *) are interchangeable via C struct aliasing rules. */
    plugin_instance plugin;

    /* Root GtkMenu widget for the popup.  NULL until the menu is first built
     * by menu_create().  Rebuilt on demand after systemmenu staleness is
     * detected or the icon theme changes. */
    GtkWidget *menu;

    /* Panel-side toolbar/button widget that triggers the menu popup.
     * Created in make_button() and added as a child of plugin->pwid.
     * Destroyed via gtk_widget_destroy() in menu_destructor(). */
    GtkWidget *bg;

    /* Icon size in pixels read from the xconf "iconsize" key during
     * construction.  Defaults to MENU_DEFAULT_ICON_SIZE.
     *
     * FIXME: after menu_constructor() runs, this field holds the same value
     * as icon_size.  One of the two fields is redundant. */
    int iconsize;

    /* Panel-reported icon size.  Currently never written after zero-init.
     * FIXME: dead field -- remove or populate from panel->max_elem_height. */
    int paneliconsize;

    /* Expanded xconf configuration tree produced by menu_expand_xc().  All
     * <systemmenu> and <include> directives have already been resolved.
     * Owned by this struct; freed with xconf_del(xc, FALSE) in menu_destroy().
     */
    xconf *xc;

    /* GLib timeout ID for the 30-second periodic system-menu staleness poll.
     * Started in menu_create() when has_system_menu is TRUE.
     * Cancelled via g_source_remove() in menu_destroy(); 0 when inactive. */
    guint tout;

    /* GLib timeout ID for the 2-second deferred menu rebuild scheduled by
     * schedule_rebuild_menu() when a change is detected.
     * Set to 0 after rebuild_menu() fires and returns FALSE. */
    guint rtout;

    /* TRUE when the expanded config contains at least one <systemmenu> node.
     * Determines whether the 30-second staleness-poll timer is started.
     * Reset to FALSE in menu_destroy(). */
    gboolean has_system_menu;

    /* Timestamp (seconds since epoch) taken when the current menu was built.
     * Passed to systemmenu_changed() to detect .desktop files newer than this
     * point on disk, triggering a scheduled rebuild. */
    time_t btime;

    /* Effective icon size in pixels used when loading item icons via
     * fb_pixbuf_new().  Populated from the "iconsize" xconf key in
     * menu_constructor().
     *
     * FIXME: identical to `iconsize` after construction; one is redundant. */
    gint icon_size;
} menu_priv;

/*
 * menu_class -- per-type descriptor/vtable for the menu plugin.
 *
 * Inherits plugin_class by embedding it as the first field.  The menu plugin
 * does not expose any additional virtual functions beyond the standard
 * constructor/destructor, so this wrapper adds nothing new.
 */
typedef struct {
    /* Base vtable -- MUST be first field (fbpanel casting convention). */
    plugin_class plugin;
} menu_class;


#endif /* MENU_H */
