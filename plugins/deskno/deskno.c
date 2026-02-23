/* deskno -- Desktop Number v1 plugin for fbpanel.
 *
 * Reused dclock.c structure and variables from pager.c.
 * 11/23/04 by cmeury@users.sf.net
 *
 * Displays the current virtual desktop number as a bold label inside a button.
 * Clicking the button or scrolling on it navigates between desktops using the
 * _NET_CURRENT_DESKTOP EWMH message.
 *
 * EWMH events are received via the global fbev GObject which emits signals
 * when the root window properties change:
 *   "current_desktop"   -> name_update()
 *   "number_of_desktops"-> update()
 *
 * Signal connection/disconnection is paired correctly: both handlers are
 * disconnected in deskno_destructor.
 *
 * No timers are used; updates are event-driven.
 */

// reused dclock.c and variables from pager.c
// 11/23/04 by cmeury@users.sf.net",

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * deskno_priv -- per-instance state.
 *
 * Memory: all widget pointers are managed by GTK.
 * No heap allocations beyond the struct itself (managed by the framework).
 */
typedef struct {
    plugin_instance plugin; /* base class -- must be first */
    GtkWidget *main;        /* GtkButton acting as the entire plugin widget   */
    GtkWidget *namew;       /* GtkLabel inside the button showing desk number */
    int deskno;             /* current (0-based) desktop index                */
    int desknum;            /* total number of desktops                       */
} deskno_priv;

/*
 * change_desktop -- switch to a neighbouring virtual desktop with wraparound.
 *
 * Parameters:
 *   dc    -- deskno_priv instance; reads dc->deskno and dc->desknum.
 *   delta -- +1 to go forward, -1 to go back.
 *
 * Sends _NET_CURRENT_DESKTOP to the root window via Xclimsg.
 * Wraps: going below 0 wraps to desknum-1; going past desknum-1 wraps to 0.
 */
static void
change_desktop(deskno_priv *dc, int delta)
{
    int newdesk = dc->deskno + delta;

    ENTER;
    // Wraparound below zero: jump to last desktop
    if (newdesk < 0)
        newdesk = dc->desknum - 1;
    // Wraparound above last: jump to first desktop
    else if (newdesk >= dc->desknum)
        newdesk = 0;
    DBG("%d/%d -> %d\n", dc->deskno, dc->desknum, newdesk);
    // Send EWMH client message to the window manager to switch desktop
    Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, newdesk, 0, 0, 0, 0);
    RET();
}

/*
 * clicked -- GtkButton "clicked" signal callback.
 *
 * Advances to the next desktop (+1).
 *
 * Parameters:
 *   widget -- the GtkButton (dc->main)
 *   dc     -- deskno_priv instance
 *
 * Signal connection: "clicked" on dc->main.
 */
static void
clicked(GtkWidget *widget, deskno_priv *dc)
{
    ENTER;
    change_desktop(dc, 1); // single click: go to next desktop
}

/*
 * scrolled -- GtkWidget "scroll-event" callback for mouse-wheel navigation.
 *
 * Parameters:
 *   widget -- the GtkButton
 *   event  -- GdkEventScroll; direction field distinguishes up/left vs down/right
 *   dc     -- deskno_priv instance
 *
 * Returns: FALSE (allows the scroll event to propagate further).
 *
 * Scroll up or left: previous desktop (-1).
 * Scroll down or right: next desktop (+1).
 *
 * Signal connection: "scroll-event" on dc->main.
 */
static gboolean
scrolled(GtkWidget *widget, GdkEventScroll *event, deskno_priv *dc)
{
    ENTER;
    change_desktop(dc, (event->direction == GDK_SCROLL_UP
            || event->direction == GDK_SCROLL_LEFT) ? -1 : 1);
    return FALSE; // propagate to allow panel-level handling if needed
}

/*
 * name_update -- refresh the displayed desktop number label.
 *
 * Parameters:
 *   widget -- ignored (required by GCallback signature compatibility)
 *   dc     -- deskno_priv instance; dc->deskno is updated.
 *
 * Returns: TRUE (required by the gint return type).
 *
 * Reads the current desktop index from EWMH and updates dc->namew label with
 * a Pango bold markup string like "<b>3</b>" (1-based for display).
 *
 * Signal connection: fbev "current_desktop" signal.
 */
static gint
name_update(GtkWidget *widget, deskno_priv *dc)
{
    char buffer [15]; /* enough for "<b>2147483647</b>\0" */

    ENTER;
    dc->deskno = get_net_current_desktop(); // read _NET_CURRENT_DESKTOP (0-based)
    // Display as 1-based, wrapped in Pango bold markup
    snprintf(buffer, sizeof(buffer), "<b>%d</b>", dc->deskno + 1);
    gtk_label_set_markup(GTK_LABEL(dc->namew), buffer);
    RET(TRUE);
}

/*
 * update -- refresh the total number of desktops.
 *
 * Parameters:
 *   widget -- ignored (required by GCallback signature)
 *   dc     -- deskno_priv instance; dc->desknum is updated.
 *
 * Returns: TRUE.
 *
 * Reads _NET_NUMBER_OF_DESKTOPS and caches it in dc->desknum so that
 * change_desktop() can perform accurate wraparound.
 *
 * Signal connection: fbev "number_of_desktops" signal.
 */
static gint
update(GtkWidget *widget, deskno_priv *dc)
{
    ENTER;
    dc->desknum = get_net_number_of_desktops();
    RET(TRUE);
}

/*
 * deskno_constructor -- set up the desktop-number plugin widget.
 *
 * Parameters:
 *   p -- plugin_instance allocated by the framework.
 *
 * Returns: 1 on success.
 *
 * Widget hierarchy:
 *   p->pwid (framework container)
 *     dc->main (GtkButton, no relief)
 *       dc->namew (GtkLabel with Pango markup)
 *
 * Signals connected (on fbev):
 *   "current_desktop"    -> name_update  (updates the number label)
 *   "number_of_desktops" -> update       (updates the desktop count)
 *
 * Both signals are disconnected in deskno_destructor.
 */
static int
deskno_constructor(plugin_instance *p)
{
    deskno_priv *dc;

    ENTER;
    dc = (deskno_priv *) p;
    // Create a flat-look button as the top-level visible widget
    dc->main = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(dc->main), GTK_RELIEF_NONE);
    g_signal_connect(G_OBJECT(dc->main), "clicked", G_CALLBACK(clicked),
        (gpointer) dc);
    g_signal_connect(G_OBJECT(dc->main), "scroll-event", G_CALLBACK(scrolled),
        (gpointer) dc);
    // Label initialised with "ww" to pre-size the button to 2-character width
    dc->namew = gtk_label_new("ww");
    gtk_container_add(GTK_CONTAINER(dc->main), dc->namew);
    gtk_container_add(GTK_CONTAINER(p->pwid), dc->main);
    //gtk_widget_add_events(p->pwid, GDK_SCROLL_MASK);   // not needed; button forwards
    //gtk_widget_add_events(dc->main, GDK_SCROLL_MASK);  // scroll events to parent
    gtk_widget_show_all(p->pwid);
    // Populate label and desktop count before the first EWMH event arrives
    name_update(dc->main, dc);
    update(dc->main, dc);
    // Subscribe to EWMH events via the global fbev GObject
    g_signal_connect(G_OBJECT(fbev), "current_desktop", G_CALLBACK
        (name_update), (gpointer) dc);
    g_signal_connect(G_OBJECT(fbev), "number_of_desktops", G_CALLBACK
        (update), (gpointer) dc);
    RET(1);
}


/*
 * deskno_destructor -- clean up the plugin on unload.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 *
 * Disconnects both fbev signal handlers registered in deskno_constructor.
 * GTK widget destruction is handled by the framework (it destroys p->pwid).
 *
 * Note: the GtkButton (dc->main) and GtkLabel (dc->namew) are children of
 * p->pwid and will be destroyed automatically when p->pwid is destroyed by
 * the framework after the destructor returns.
 */
static void
deskno_destructor(plugin_instance *p)
{
  deskno_priv *dc = (deskno_priv *) p;

  ENTER;
  // Disconnect EWMH event handlers to stop receiving stale callbacks
  g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), name_update, dc);
  g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), update, dc);
  RET();
}

/* Plugin class descriptor */
static plugin_class class = {
    .type        = "deskno",
    .name        = "Desktop No v1",
    .version     = "0.6",
    .description = "Display workspace number",
    .priv_size   = sizeof(deskno_priv),

    .constructor = deskno_constructor,
    .destructor  = deskno_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
