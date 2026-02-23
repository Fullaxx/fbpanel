/* deskno2 -- Desktop Number v2 plugin for fbpanel.
 *
 * Display workspace number by cmeury@users.sf.net.
 *
 * An improved version of deskno that shows the desktop *name* (from
 * _NET_DESKTOP_NAMES) rather than just the number. Falls back to the
 * 1-based numeric index for desktops that have no name.
 *
 * Clicking the button launches xfce-setting-show workspaces to open the
 * workspace configuration dialog (hard-coded XFCE dependency -- see BUG).
 *
 * Scroll wheel cycles through desktops; scroll up = previous, down = next.
 *
 * EWMH signals (via fbev):
 *   "current_desktop"    -> update_dno    (update displayed name for new desk)
 *   "desktop_names"      -> update_all    (rebuild name list when names change)
 *   "number_of_desktops" -> update_all    (rebuild when desktop count changes)
 *
 * Memory:
 *   dc->dnames -- g_strfreev'd on each update_all call and in destructor.
 *   dc->lnames -- same; NULL-terminated array allocated with g_new0.
 */

/* Display workspace number, by cmeury@users.sf.net */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * deskno_priv -- per-instance state for deskno2.
 *
 * Memory ownership:
 *   dnames  -- NULL-terminated array of UTF-8 strings from X property;
 *              allocated by get_utf8_property_list, freed with g_strfreev.
 *   lnames  -- NULL-terminated array of display-ready name strings;
 *              each element is g_strdup'd, freed with g_strfreev.
 *   fmt     -- unused field (reserved for future format string support).
 */
typedef struct {
    plugin_instance plugin;  /* base class -- must be first */
    GtkWidget  *main;        /* GtkButton showing current desktop name      */
    int         dno;         /* current desktop index (0-based)             */
    int         dnum;        /* total number of desktops                    */
    char      **dnames;      /* desktop names from _NET_DESKTOP_NAMES       */
    int         dnames_num;  /* number of entries in dnames                 */
    char      **lnames;      /* label strings: name if available, else "N"  */
    char       *fmt;         /* reserved / unused                           */
} deskno_priv;

/*
 * clicked -- GtkButton "clicked" callback.
 *
 * Launches the XFCE workspace settings dialog via system().
 *
 * BUG: hard-coded XFCE tool "xfce-setting-show workspaces" -- will silently
 *      fail (and block briefly) on non-XFCE desktops. system() is also a
 *      security risk if PATH is compromised. Return value is ignored.
 *
 * Parameters:
 *   widget -- the GtkButton
 *   dc     -- deskno_priv instance (unused in this implementation)
 */
static  void
clicked(GtkWidget *widget, deskno_priv *dc)
{
    // BUG: system() blocks the GTK main loop until the command finishes,
    //      and return value (child exit code) is ignored entirely.
    (void)system("xfce-setting-show workspaces");
}

/*
 * update_dno -- update the button label to show the current desktop's name.
 *
 * Parameters:
 *   widget -- ignored (GCallback compatibility)
 *   dc     -- deskno_priv instance; dc->dno is updated from fbev.
 *
 * Reads fb_ev_current_desktop() (which caches the last EWMH event value)
 * then sets the button label to dc->lnames[dc->dno].
 *
 * Precondition: dc->lnames must be non-NULL and have at least dc->dnum entries.
 *               If dc->dno ever exceeds dc->dnames_num this could access
 *               uninitialised memory -- see BUG entry.
 *
 * Signal connection: fbev "current_desktop".
 */
static  void
update_dno(GtkWidget *widget, deskno_priv *dc)
{
    ENTER;
    dc->dno = fb_ev_current_desktop(fbev); // read cached current desktop index
    // Use the pre-built label array; dc->dno should always be < dc->dnum
    gtk_button_set_label(GTK_BUTTON(dc->main), dc->lnames[dc->dno]);

    RET();
}

/*
 * update_all -- rebuild desktop names and label array, then refresh display.
 *
 * Parameters:
 *   widget -- ignored
 *   dc     -- deskno_priv instance.
 *
 * Called when the number of desktops or desktop names change.
 * Frees old dnames/lnames, re-fetches from X, builds lnames as:
 *   - dc->dnames[i] if i < dc->dnames_num (actual name)
 *   - "%d" (1-based numeric) for any desktop without a name
 *
 * Then calls update_dno to refresh the displayed button label.
 *
 * Signals: fbev "desktop_names" and "number_of_desktops".
 */
static  void
update_all(GtkWidget *widget, deskno_priv *dc)
{
    int i;

    ENTER;
    dc->dnum = fb_ev_number_of_desktops(fbev); // refresh total desktop count
    // Free the old name arrays before reallocating
    if (dc->dnames)
        g_strfreev (dc->dnames);
    if (dc->lnames)
        g_strfreev (dc->lnames);
    // Fetch _NET_DESKTOP_NAMES from the root window; sets dc->dnames_num
    dc->dnames = get_utf8_property_list(GDK_ROOT_WINDOW(), a_NET_DESKTOP_NAMES, &(dc->dnames_num));
    // Allocate the display label array (+1 for the NULL sentinel)
    dc->lnames = g_new0 (gchar*, dc->dnum + 1);
    // Use actual desktop names for desktops that have them
    for (i = 0; i < MIN(dc->dnum, dc->dnames_num); i++) {
        dc->lnames[i] = g_strdup(dc->dnames[i]);
    }
    // Fall back to numeric string for desktops with no name
    for (; i < dc->dnum; i++) {
        dc->lnames[i] = g_strdup_printf("%d", i + 1); // 1-based
    }
    // Update the button label to reflect the current desktop
    update_dno(widget, dc);
    RET();
}


/*
 * scroll -- GtkWidget "scroll-event" callback; navigates desktops.
 *
 * Parameters:
 *   widget -- the GtkButton
 *   event  -- GdkEventScroll
 *   dc     -- deskno_priv instance
 *
 * Returns: TRUE (event consumed).
 *
 * Scroll up: go to previous desktop; scroll down: go to next.
 * Wraps at both ends.
 *
 * Note: GDK_SCROLL_LEFT/RIGHT are not handled here (only UP/DOWN).
 * Contrast with deskno v1 which handles all four directions.
 */
static gboolean
scroll (GtkWidget *widget, GdkEventScroll *event, deskno_priv *dc)
{
    int dno;

    ENTER;
    // Calculate target desktop with direction-based delta
    dno = dc->dno + ((event->direction == GDK_SCROLL_UP) ? (-1) : (+1));
    // Wraparound: below 0 -> last desktop; above dnum-1 -> first
    if (dno < 0)
        dno = dc->dnum - 1;
    else if (dno == dc->dnum)
        dno = 0;
    // Send _NET_CURRENT_DESKTOP EWMH message to the window manager
    Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, dno, 0, 0, 0, 0);
    RET(TRUE);

}

/*
 * deskno_constructor -- initialise the deskno2 plugin.
 *
 * Parameters:
 *   p -- plugin_instance allocated by the framework.
 *
 * Returns: 1 on success.
 *
 * Widget hierarchy:
 *   p->pwid (framework container)
 *     dc->main (GtkButton, no relief, initial label "w")
 *
 * Signals connected on fbev:
 *   "current_desktop"    -> update_dno
 *   "desktop_names"      -> update_all
 *   "number_of_desktops" -> update_all
 *
 * All three are disconnected in deskno_destructor.
 */
static int
deskno_constructor(plugin_instance *p)
{
    deskno_priv *dc;
    ENTER;
    dc = (deskno_priv *) p;
    // Button with an initial single-char label to set a minimal width
    dc->main = gtk_button_new_with_label("w");
    gtk_button_set_relief(GTK_BUTTON(dc->main),GTK_RELIEF_NONE);
    gtk_container_set_border_width(GTK_CONTAINER(dc->main), 0);
    //gtk_button_set_alignment(GTK_BUTTON(dc->main), 0, 0.5); // left-align text (disabled)
    g_signal_connect(G_OBJECT(dc->main), "clicked", G_CALLBACK (clicked), (gpointer) dc);
    g_signal_connect(G_OBJECT(dc->main), "scroll-event", G_CALLBACK(scroll), (gpointer) dc);

    // Build name arrays and show initial desktop name immediately
    update_all(dc->main, dc);

    gtk_container_add(GTK_CONTAINER(p->pwid), dc->main);
    gtk_widget_show_all(p->pwid);

    // Subscribe to EWMH change events
    g_signal_connect (G_OBJECT (fbev), "current_desktop", G_CALLBACK (update_dno), (gpointer) dc);
    g_signal_connect (G_OBJECT (fbev), "desktop_names", G_CALLBACK (update_all), (gpointer) dc);
    g_signal_connect (G_OBJECT (fbev), "number_of_desktops", G_CALLBACK (update_all), (gpointer) dc);

    RET(1);
}


/*
 * deskno_destructor -- release resources on plugin unload.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 *
 * Disconnects fbev signal handlers and frees the desktop name arrays.
 * GTK widgets are freed by the framework (p->pwid cascade).
 */
static void
deskno_destructor(plugin_instance *p)
{
    deskno_priv *dc = (deskno_priv *) p;

    ENTER;
    /* disconnect ALL handlers matching func and data */
    g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), update_dno, dc);
    g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), update_all, dc);
    // Free the heap-allocated name arrays
    if (dc->dnames)
        g_strfreev(dc->dnames);
    if (dc->lnames)
        g_strfreev(dc->lnames);
    RET();
}

/* Plugin class descriptor */
static plugin_class class = {
    .count       = 0,
    .type        = "deskno2",
    .name        = "Desktop No v2",
    .version     = "0.6",
    .description = "Display workspace number",
    .priv_size   = sizeof(deskno_priv),

    .constructor = deskno_constructor,
    .destructor  = deskno_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
