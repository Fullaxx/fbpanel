/*
 * plugins/menu/menu.c -- Implementation of the fbpanel "menu" plugin.
 *
 * PURPOSE
 * -------
 * Provides an application-menu button on the panel.  Clicking the button
 * pops up a hierarchical GtkMenu built from an xconf configuration tree.
 * The tree may include:
 *   - Static items (<item> with an <action> command or icon/image).
 *   - Separators (<separator>).
 *   - Submenus (<menu>).
 *   - A dynamically generated system application menu (<systemmenu>),
 *     built by scanning XDG .desktop files (see system_menu.c).
 *   - Included external config files (<include file="...">).
 *
 * The menu is built lazily (first button press) and is automatically
 * rebuilt when:
 *   1. The GTK icon theme changes (schedule_rebuild_menu via signal).
 *   2. The XDG application directories change on disk (check_system_menu
 *      polls every 30 s when a <systemmenu> is present).
 *
 * AUTOHIDE INTERACTION
 * --------------------
 * When the panel uses autohide, the panel is kept visible while the menu
 * is open (ah_stop) and re-enables autohide on menu unmap (ah_start).
 *
 * PUBLIC API (to the plugin framework)
 * -------------------------------------
 *   class_ptr  (file-scope static)  -- plugin_class* registered with the
 *                                      panel loader at link time.
 *
 * EXTERNAL SYMBOLS CONSUMED (from system_menu.c)
 * -----------------------------------------------
 *   xconf_new_from_systemmenu()  -- build xconf tree from XDG .desktop files
 *   systemmenu_changed(btime)    -- check whether .desktop files changed
 */

#include <stdlib.h>
#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "bg.h"
#include "gtkbgbox.h"
#include "run.h"
#include "menu.h"

//#define DEBUGPRN
#include "dbg.h"

/* Forward declarations for functions defined later in this file. */
xconf *xconf_new_from_systemmenu();
gboolean systemmenu_changed(time_t btime);
static void menu_create(plugin_instance *p);
static void menu_destroy(menu_priv *m);
static gboolean check_system_menu(plugin_instance *p);

/*
 * menu_expand_xc -- recursively expand a raw xconf tree into a resolved tree.
 *
 * Walks the @xc tree depth-first and produces a new xconf tree (@nxc) where:
 *   - <systemmenu> nodes are replaced by the dynamically generated system
 *     menu (from xconf_new_from_systemmenu()).
 *   - <include value="path"> nodes are replaced by the tree loaded from
 *     that file (xconf_new_from_file()).
 *   - All other nodes are deep-copied recursively.
 *
 * Parameters:
 *   xc  -- source xconf node (read-only, not modified).
 *   m   -- current menu_priv instance; m->has_system_menu is set to TRUE
 *          if a <systemmenu> node is encountered.
 *
 * Returns:
 *   A newly allocated xconf tree.  The caller owns the returned tree and
 *   must free it with xconf_del(result, FALSE) when done.
 *   Returns NULL if @xc is NULL.
 *
 * Memory notes:
 *   - smenu_xc returned by xconf_new_from_systemmenu() / xconf_new_from_file()
 *     is appended into @nxc (transferring ownership of children) and then freed
 *     with xconf_del(smenu_xc, FALSE) -- the FALSE means "do not free children
 *     recursively", which is correct since children were moved.
 *   - Recursive calls for non-special nodes are via menu_expand_xc(), whose
 *     return value is appended directly into @nxc (ownership transferred).
 */
static xconf *
menu_expand_xc(xconf *xc, menu_priv *m)
{
    xconf *nxc, *cxc, *smenu_xc;
    GSList *w;

    ENTER;
    if (!xc)
        RET(NULL);

    /* Create a new node mirroring the current node (copies name & value). */
    nxc = xconf_new(xc->name, xc->value);
    DBG("new node:%s\n", nxc->name);

    /* Iterate over all children of the source node. */
    for (w = xc->sons; w; w = g_slist_next(w))
    {
        cxc = w->data;

        /* <systemmenu> -- replace with dynamically generated system menu. */
        if (!strcmp(cxc->name, "systemmenu"))
        {
            smenu_xc = xconf_new_from_systemmenu();
            /* Move children of smenu_xc into nxc, then free the wrapper node. */
            xconf_append_sons(nxc, smenu_xc);
            xconf_del(smenu_xc, FALSE); /* FALSE = do not free already-moved children */
            m->has_system_menu = TRUE;
            continue;
        }

        /* <include value="path"> -- load and inline an external config file. */
        if (!strcmp(cxc->name, "include"))
        {
            smenu_xc = xconf_new_from_file(cxc->value, "include");
            xconf_append_sons(nxc, smenu_xc);
            xconf_del(smenu_xc, FALSE); /* FALSE = children already moved */
            continue;
        }

        /* All other nodes: deep-copy recursively and attach. */
        xconf_append(nxc, menu_expand_xc(cxc, m));
    }
    return nxc;
}

#if 0
/* XXX: should be global service with following API
 * register_command, unregister_command, run_command
 *
 * FIXME: The command dispatch API was stubbed out but never implemented.
 * The <command> xconf key in menu items is parsed but has no effect.
 */
static void
run_command(GtkWidget *widget, void (*cmd)(void))
{
    ENTER;
    cmd();
    RET();
}
#endif

/*
 * menu_create_separator -- create a horizontal GtkSeparatorMenuItem.
 *
 * Returns:
 *   A new GtkSeparatorMenuItem widget.  Ownership is transferred to the
 *   caller (typically added to a GtkMenuShell which takes a reference).
 */
static GtkWidget *
menu_create_separator()
{
    return gtk_separator_menu_item_new();
}

/*
 * menu_create_item -- create a single GtkImageMenuItem from an xconf node.
 *
 * Reads the following child keys from @xc:
 *   "name"   -- label text for the menu item (optional; empty string if absent)
 *   "image"  -- file path to an icon image (~ expansion applied)
 *   "icon"   -- icon name for the GTK icon theme
 *   "action" -- shell command to run when the item is activated (~ expansion)
 *   "command" -- built-in command name (currently unimplemented/dead code)
 *
 * If @menu is non-NULL the item becomes a submenu trigger (the submenu widget
 * is attached with gtk_menu_item_set_submenu) and "action"/"command" are not
 * read.
 *
 * Parameters:
 *   xc   -- xconf node containing the item configuration; not modified.
 *   menu -- optional pre-built GtkMenu to attach as a submenu; may be NULL.
 *   m    -- current menu_priv instance (used for m->icon_size).
 *
 * Returns:
 *   A new GtkImageMenuItem widget.  Ownership is transferred to the caller.
 *
 * Memory notes:
 *   - fname is the result of expand_tilda(XCG result); freed via g_free() at
 *     the end of this function.
 *   - action after expand_tilda() is owned by the GObject data slot "activate"
 *     with g_free as the destroy notifier; it is NOT freed here explicitly.
 *   - The GdkPixbuf pb is unreferenced after being set on the widget (the
 *     widget holds its own reference).
 *
 * BUG: fname is freed unconditionally at the end even when it is NULL
 *      (expand_tilda returns NULL when given NULL).  g_free(NULL) is safe in
 *      GLib, but the pattern is misleading and the NULL guard is missing.
 *
 * BUG: When @menu is NULL and neither "action" nor "command" is present,
 *      the item is returned with no signal handler.  Clicking it does nothing,
 *      with no error or warning to the user.
 *
 * FIXME: The "command" branch is entirely disabled (#if 0 inside).  The xconf
 *        "command" key is silently ignored.
 */
static GtkWidget *
menu_create_item(xconf *xc, GtkWidget *menu, menu_priv *m)
{
    gchar *name, *fname, *iname, *action, *cmd;
    GtkWidget *mi;

    /* Zero-init all local string pointers before XCG calls. */
    cmd = name = fname = action = iname = NULL;

    XCG(xc, "name", &name, str);
    mi = gtk_image_menu_item_new_with_label(name ? name : "");
    gtk_container_set_border_width(GTK_CONTAINER(mi), 0);

    /* Load image: prefer a file path ("image"), fall back to theme icon name ("icon"). */
    XCG(xc, "image", &fname, str);
    fname = expand_tilda(fname); /* expand_tilda handles NULL input, returning NULL */
    XCG(xc, "icon", &iname, str);
    if (fname || iname)
    {
        GdkPixbuf *pb;

        /* fb_pixbuf_new tries iname first (theme lookup) then fname (file). */
        if ((pb = fb_pixbuf_new(iname, fname, m->icon_size, m->icon_size,
                    FALSE)))
        {
            gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi),
                    gtk_image_new_from_pixbuf(pb));
            /* Widget took its own ref; release ours. */
            g_object_unref(G_OBJECT(pb));
        }
    }
    /* Free the expanded file-path string (iname points into xconf, not ours). */
    g_free(fname);

    if (menu)
    {
        /* This item is a submenu entry: attach the provided GtkMenu. */
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
        goto done;
    }

    /* Check for an external shell command action. */
    XCG(xc, "action", &action, str);
    if (action)
    {
        action = expand_tilda(action); /* ownership of returned string moves below */

        /* run_app is called with `action` as argument when item is activated. */
        g_signal_connect_swapped(G_OBJECT(mi), "activate",
                (GCallback)run_app, action);
        /* Store `action` string as object data; g_free is the destroy handler,
         * so the string is freed when the widget is destroyed. */
        g_object_set_data_full(G_OBJECT(mi), "activate",
            action, g_free);
        goto done;
    }

    /* Check for a named built-in command. */
    XCG(xc, "command", &cmd, str);
    if (cmd)
    {
        /* XXX: implement command API */
#if 0
        /* FIXME: This block is permanently disabled.  The command lookup
         * table and dispatch were never hooked up. */
        command *tmp;

        for (tmp = commands; tmp->name; tmp++)
            if (!g_ascii_strcasecmp(cmd, tmp->name))
            {
                g_signal_connect(G_OBJECT(mi), "activate",
                        (GCallback)run_command, tmp->cmd);
                goto done;
            }
#endif
    }

done:
    return mi;
}

/*
 * menu_create_menu -- recursively build a GtkMenu from an xconf node.
 *
 * Iterates over the children of @xc, creating:
 *   - <separator>  -> GtkSeparatorMenuItem via menu_create_separator()
 *   - <item>       -> GtkImageMenuItem via menu_create_item(..., NULL, m)
 *   - <menu>       -> recursive call; result is a GtkImageMenuItem with submenu
 *
 * Parameters:
 *   xc       -- xconf node whose children describe the menu contents.
 *   ret_menu -- if TRUE, return the raw GtkMenu widget.
 *               if FALSE, wrap the GtkMenu as a submenu of a new item (the
 *               item label/icon are taken from @xc itself).
 *   m        -- current menu_priv (forwarded to menu_create_item()).
 *
 * Returns:
 *   If ret_menu is TRUE: the GtkMenu widget.
 *   If ret_menu is FALSE: a GtkImageMenuItem that pops up the menu as submenu.
 *   Returns NULL if @xc is NULL.
 *   Ownership is transferred to the caller in both cases.
 *
 * Side effects:
 *   gtk_widget_show_all() is called on the GtkMenu before returning.
 */
static GtkWidget *
menu_create_menu(xconf *xc, gboolean ret_menu, menu_priv *m)
{
    GtkWidget *mi, *menu;
    GSList *w;
    xconf *nxc;

    if (!xc)
        return NULL;

    menu = gtk_menu_new();
    gtk_container_set_border_width(GTK_CONTAINER(menu), 0);

    /* Walk all children of the xconf node and append corresponding widgets. */
    for (w = xc->sons; w ; w = g_slist_next(w))
    {
        nxc = w->data;
        if (!strcmp(nxc->name, "separator"))
            mi = menu_create_separator();
        else if (!strcmp(nxc->name, "item"))
            mi = menu_create_item(nxc, NULL, m);
        else if (!strcmp(nxc->name, "menu"))
            /* Recurse; ret_menu=FALSE returns a submenu-trigger item. */
            mi = menu_create_menu(nxc, FALSE, m);
        else
            continue; /* skip unknown/internal nodes (e.g., "name", "icon") */
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    gtk_widget_show_all(menu);

    /* Caller determines whether they want the raw menu or a wrapper item. */
    return (ret_menu) ? menu : menu_create_item(xc, menu, m);
}

/*
 * menu_unmap -- signal handler called when the popup menu is unmapped.
 *
 * Re-enables panel autohide (ah_start) after the menu is closed.
 *
 * Parameters:
 *   menu -- the GtkMenu widget being unmapped (unused).
 *   p    -- plugin_instance* for this menu plugin.
 *
 * Returns:
 *   FALSE (allows the "unmap" signal to propagate further).
 */
static gboolean
menu_unmap(GtkWidget *menu, plugin_instance *p)
{
    ENTER;
    if (p->panel->autohide)
        ah_start(p->panel); /* resume autohide now that menu is gone */
    RET(FALSE);
}

/*
 * menu_create -- (re)build the popup GtkMenu from the xconf configuration.
 *
 * If a menu already exists it is destroyed first (menu_destroy).
 * Expands the raw plugin xconf (p->xc) into a resolved tree (m->xc) and
 * then builds the GTK widget hierarchy from it.
 *
 * If the expanded config contains a <systemmenu> node, a 30-second polling
 * timer is started to detect .desktop file changes.
 *
 * Parameters:
 *   p -- plugin_instance* (cast to menu_priv* internally).
 *
 * Side effects:
 *   Sets m->menu, m->xc, m->has_system_menu, m->btime, m->tout.
 *   Connects the "unmap" signal on m->menu to menu_unmap().
 */
static void
menu_create(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    ENTER;
    /* Destroy any previously built menu and free its resources. */
    if (m->menu)
        menu_destroy(m);

    /* Expand raw xconf (resolving <systemmenu> and <include>) into m->xc. */
    m->xc = menu_expand_xc(p->xc, m);

    /* Build GTK menu hierarchy from the expanded tree. */
    m->menu = menu_create_menu(m->xc, TRUE, m);

    /* Re-enable autohide when the menu is dismissed. */
    g_signal_connect(G_OBJECT(m->menu), "unmap",
        G_CALLBACK(menu_unmap), p);

    /* Record the build timestamp for staleness detection. */
    m->btime = time(NULL);

    /* If the config included a <systemmenu>, poll for .desktop changes. */
    if (m->has_system_menu)
        m->tout = g_timeout_add(30000, (GSourceFunc) check_system_menu, p);
    RET();
}

/*
 * menu_destroy -- tear down the popup GtkMenu and cancel associated timers.
 *
 * Does NOT destroy the panel button widget (m->bg); that is handled separately
 * in menu_destructor().
 *
 * Parameters:
 *   m -- menu_priv instance to clean up.
 *
 * Side effects:
 *   Destroys m->menu widget, removes m->tout and m->rtout GLib timers,
 *   frees m->xc.  All four fields are set to 0/NULL afterwards.
 */
static void
menu_destroy(menu_priv *m)
{
    ENTER;
    if (m->menu) {
        gtk_widget_destroy(m->menu);
        m->menu = NULL;
        m->has_system_menu = FALSE; /* reset so timer is not re-armed without reason */
    }
    if (m->tout) {
        g_source_remove(m->tout);   /* cancel 30-second staleness check */
        m->tout = 0;
    }
    if (m->rtout) {
        g_source_remove(m->rtout);  /* cancel 2-second deferred rebuild */
        m->rtout = 0;
    }
    if (m->xc) {
        xconf_del(m->xc, FALSE);    /* free xconf wrapper node (children already owned) */
        m->xc = NULL;
    }
    RET();
}

/*
 * my_button_pressed -- GdkEventButton handler for the panel toolbar button.
 *
 * Pops up the menu on a standard button press that is within the widget
 * bounds.  Control+right-click is propagated to the panel (for panel context
 * menus) rather than popping up the application menu.
 *
 * Parameters:
 *   widget -- the button GtkWidget that received the event.
 *   event  -- the GdkEventButton describing the press.
 *   p      -- plugin_instance* for this menu plugin.
 *
 * Returns:
 *   TRUE  -- event consumed (stops further propagation).
 *   FALSE -- event propagated (only for Control+Button3).
 *
 * Side effects:
 *   May call menu_create() if m->menu is NULL.
 *   Calls ah_stop() to prevent panel autohide while the menu is open.
 *   Calls gtk_menu_popup() to display the menu.
 */
static gboolean
my_button_pressed(GtkWidget *widget, GdkEventButton *event, plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    ENTER;
    /* Propagate Control+Button3 to the panel for its own context menu. */
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
        && event->state & GDK_CONTROL_MASK)
    {
        RET(FALSE);
    }

    /* Only react to clicks that land inside the widget boundaries. */
    if ((event->type == GDK_BUTTON_PRESS)
        && (event->x >=0 && event->x < widget->allocation.width)
        && (event->y >=0 && event->y < widget->allocation.height))
    {
        /* Build the menu lazily on first use. */
        if (!m->menu)
            menu_create(p);

        /* Prevent panel from hiding while the menu is visible. */
        if (p->panel->autohide)
            ah_stop(p->panel);

        /* Display the menu anchored relative to the panel button. */
        gtk_menu_popup(GTK_MENU(m->menu),
            NULL, NULL, (GtkMenuPositionFunc)menu_pos, widget,
            event->button, event->time);
    }
    RET(TRUE);
}

/*
 * make_button -- create and add the panel toolbar button for the menu plugin.
 *
 * Reads image/icon configuration from @xc and creates a fb_button widget.
 * The button is added as a child of the plugin container (p->pwid).
 * The "button-press-event" signal is connected to my_button_pressed().
 *
 * Parameters:
 *   p  -- plugin_instance* (cast to menu_priv* for m->bg).
 *   xc -- xconf node to read "image" and "icon" keys from.
 *
 * Side effects:
 *   Sets m->bg.
 *   If the panel is transparent, sets background inheritance on m->bg.
 *
 * Memory notes:
 *   fname is freed with g_free() at the end (expand_tilda returns heap memory).
 *   iname points into xconf memory and is not freed here.
 *
 * FIXME: The XXX comment inside this function notes that the width/height
 *        computation (w=-1, h=max_elem_height for horizontal) is duplicated in
 *        every plugin -- it should be centralised in the panel.
 *
 * BUG: If neither "image" nor "icon" is configured, m->bg is never set and
 *      remains NULL (zeroed by g_malloc0).  menu_destructor() then calls
 *      gtk_widget_destroy(m->bg) unconditionally, which will crash with a
 *      NULL pointer dereference.
 */
static void
make_button(plugin_instance *p, xconf *xc)
{
    int w, h;
    menu_priv *m;
    gchar *fname, *iname;

    ENTER;
    m = (menu_priv *) p;

    /* XXX: this code is duplicated in every plugin.
     * Lets run it once in a panel */
    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL)
    {
        w = -1;                       /* natural width */
        h = p->panel->max_elem_height;
    }
    else
    {
        w = p->panel->max_elem_height;
        h = -1;                       /* natural height */
    }

    fname = iname = NULL;
    XCG(xc, "image", &fname, str);
    fname = expand_tilda(fname);
    XCG(xc, "icon", &iname, str);
    if (fname || iname)
    {
        /* 0x702020 is a fallback highlight colour (dark red). */
        m->bg = fb_button_new(iname, fname, w, h, 0x702020, NULL);
        gtk_container_add(GTK_CONTAINER(p->pwid), m->bg);
        if (p->panel->transparent)
            gtk_bgbox_set_background(m->bg, BG_INHERIT, 0, 0);
        g_signal_connect(G_OBJECT(m->bg), "button-press-event",
            G_CALLBACK(my_button_pressed), p);
    }
    g_free(fname); /* free expand_tilda result; g_free(NULL) is safe */
}

/*
 * rebuild_menu -- GSourceFunc: rebuild the menu if it is not currently mapped.
 *
 * Called by the 2-second GLib timer set up in schedule_rebuild_menu().
 * If the menu is visible (mapped) the rebuild is deferred by returning TRUE.
 * Otherwise menu_create() is called and FALSE is returned so that the timer
 * is removed (one-shot behaviour).
 *
 * Parameters:
 *   p -- plugin_instance* passed as the GSourceFunc gpointer.
 *
 * Returns:
 *   TRUE  -- keep the timer active (menu is currently open; try again later).
 *   FALSE -- timer has fired and done its work; remove the source.
 *
 * Side effects:
 *   Resets m->rtout to 0 after the rebuild so schedule_rebuild_menu() knows
 *   the slot is free for a new one-shot timer.
 */
static gboolean
rebuild_menu(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    ENTER;
    /* Do not rebuild while the menu popup is visible. */
    if (m->menu && GTK_WIDGET_MAPPED(m->menu))
        RET(TRUE);          /* keep timer alive; retry next tick */

    menu_create(p);
    m->rtout = 0;           /* clear handle so a future schedule is allowed */
    RET(FALSE);             /* one-shot: remove the timer */
}

/*
 * schedule_rebuild_menu -- schedule a deferred 2-second menu rebuild.
 *
 * Called from the icon-theme-changed signal and from check_system_menu().
 * Ensures at most one deferred rebuild is queued at a time (idempotent).
 *
 * Parameters:
 *   p -- plugin_instance* (cast to menu_priv* internally).
 *
 * Side effects:
 *   Sets m->rtout to the GLib timeout source ID if no rebuild is pending.
 */
static void
schedule_rebuild_menu(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    ENTER;
    /* Guard against scheduling multiple concurrent rebuild timers. */
    if (!m->rtout) {
        DBG("scheduling menu rebuild p=%p\n", p);
        m->rtout = g_timeout_add(2000, (GSourceFunc) rebuild_menu, p);
    }
    RET();
}

/*
 * check_system_menu -- GSourceFunc: check whether the system menu is stale.
 *
 * Called every 30 seconds via the GLib timer started in menu_create() when
 * the expanded config included a <systemmenu> node.  If any XDG application
 * directory has changed since the menu was built (m->btime), a deferred
 * rebuild is scheduled via schedule_rebuild_menu().
 *
 * Parameters:
 *   p -- plugin_instance* passed as gpointer by g_timeout_add().
 *
 * Returns:
 *   TRUE -- always; the 30-second timer must remain active.
 */
static gboolean
check_system_menu(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    ENTER;
    if (systemmenu_changed(m->btime))
        schedule_rebuild_menu(p);

    RET(TRUE); /* keep periodic timer running */
}

/*
 * menu_constructor -- plugin_class.constructor: initialise the menu plugin.
 *
 * Reads configuration from p->xc (icon size), creates the panel button via
 * make_button(), connects the icon-theme-changed signal, and schedules the
 * initial menu build (deferred 2 seconds so the panel finishes layout first).
 *
 * Parameters:
 *   p -- plugin_instance* allocated by the panel framework.
 *
 * Returns:
 *   1 on success (non-zero as required by plugin_class.constructor contract).
 *
 * Side effects:
 *   Populates m->icon_size, m->bg.
 *   Connects schedule_rebuild_menu to the global icon_theme "changed" signal.
 *   Schedules rebuild_menu via g_timeout_add(2000).
 */
static int
menu_constructor(plugin_instance *p)
{
    menu_priv *m;

    ENTER;
    m = (menu_priv *) p;

    /* Set icon size from config, defaulting to MENU_DEFAULT_ICON_SIZE. */
    m->icon_size = MENU_DEFAULT_ICON_SIZE;
    XCG(p->xc, "iconsize", &m->icon_size, int);
    DBG("icon_size=%d\n", m->icon_size);

    /* Create the panel toolbar button widget. */
    make_button(p, p->xc);

    /* Rebuild the menu whenever the GTK icon theme changes. */
    g_signal_connect_swapped(G_OBJECT(icon_theme),
        "changed", (GCallback) schedule_rebuild_menu, p);

    /* Schedule the first menu build after a short delay. */
    schedule_rebuild_menu(p);
    RET(1);
}

/*
 * menu_destructor -- plugin_class.destructor: release all plugin resources.
 *
 * Disconnects the icon-theme signal, tears down the popup menu, and destroys
 * the panel button widget.  Called by the panel framework before the
 * plugin_instance is freed.
 *
 * Parameters:
 *   p -- plugin_instance* being destroyed.
 *
 * Side effects:
 *   Disconnects icon_theme signal.
 *   Calls menu_destroy() to free menu, timers, and xconf tree.
 *   Destroys m->bg widget.
 *
 * BUG: gtk_widget_destroy(m->bg) is called unconditionally.  If make_button()
 *      never set m->bg (because neither "image" nor "icon" was configured in
 *      xconf), this is a NULL pointer dereference / crash.
 */
static void
menu_destructor(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    ENTER;
    /* Disconnect the icon-theme change signal before any teardown. */
    g_signal_handlers_disconnect_by_func(G_OBJECT(icon_theme),
        schedule_rebuild_menu, p);

    /* Destroy the popup menu, cancel timers, free xconf tree. */
    menu_destroy(m);

    /* Destroy the panel button widget.
     * BUG: crashes if m->bg is NULL (no image/icon configured). */
    gtk_widget_destroy(m->bg);
    RET();
}


/*
 * class -- static plugin descriptor for the "menu" plugin.
 *
 * This is the single instance of menu_class for this plugin type.
 * The panel loader discovers it via the class_ptr variable below.
 */
static menu_class class = {
    .plugin = {
        .count       = 0,          /* reference count; managed by plugin.c */
        .type        = "menu",     /* registry key */
        .name        = "Menu",     /* human-readable display name */
        .version     = "1.0",
        .description = "Menu",
        .priv_size   = sizeof(menu_priv), /* bytes to allocate per instance */

        .constructor = menu_constructor,
        .destructor  = menu_destructor,
    }
};

/* class_ptr -- exported symbol used by the panel plugin loader to find the
 * class descriptor.  The loader scans for a "plugin_class *class_ptr" symbol
 * in each plugin module. */
static plugin_class *class_ptr = (plugin_class *) &class;
