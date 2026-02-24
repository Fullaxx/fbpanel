/*
 * xkill.c -- fbpanel window killer plugin.
 *
 * Click the panel button to enter "kill mode": the cursor changes to a
 * skull/crosshair.  Click any window to send XKillClient() for that
 * window's owning client.  Press Escape or right-click to cancel.
 *
 * No new library dependencies -- uses X11 and GDK which are already linked.
 *
 * Configuration (xconf keys):
 *   none
 */

#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <gdk/gdkx.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

typedef struct {
    plugin_instance  plugin;
    GtkWidget       *button;     /* the visible panel button */
    gboolean         active;     /* TRUE while in kill-mode */
    Cursor           kill_cursor;
} xkill_priv;

/* Forward declaration. */
static GdkFilterReturn xkill_event_filter(GdkXEvent *gxev, GdkEvent *event,
                                          gpointer data);

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Walk up the X window tree to find the direct child of the root window.
 * XKillClient() works on any window in the target client, but using the
 * top-level avoids killing subwindow resources of unrelated clients when
 * subwindow nesting is unusual. */
static Window
xkill_toplevel(Display *dpy, Window win)
{
    Window root, parent;
    Window *children = NULL;
    unsigned int nchildren;
    while (1) {
        if (!XQueryTree(dpy, win, &root, &parent,
                        &children, &nchildren))
            break;
        if (children)
            XFree(children);
        if (parent == root || parent == None)
            break;
        win = parent;
    }
    return win;
}

static void
xkill_cancel(xkill_priv *priv)
{
    if (!priv->active)
        return;
    priv->active = FALSE;
    gdk_pointer_ungrab(GDK_CURRENT_TIME);
    gdk_window_remove_filter(gdk_get_default_root_window(),
                             (GdkFilterFunc) xkill_event_filter, priv);
    gtk_button_set_label(GTK_BUTTON(priv->button), "Kill");
    DBG("xkill: cancelled\n");
}

/* GDK event filter installed while in kill mode.
 * Intercepts the next ButtonPress on the root: determines the clicked
 * window, kills it, then exits kill mode. */
static GdkFilterReturn
xkill_event_filter(GdkXEvent *gxev, GdkEvent *event, gpointer data)
{
    xkill_priv *priv = (xkill_priv *) data;
    XEvent *xev = (XEvent *) gxev;
    Display *dpy = GDK_DISPLAY();

    if (!priv->active)
        return GDK_FILTER_CONTINUE;

    if (xev->type == ButtonPress) {
        if (xev->xbutton.button != 1) {
            /* Any non-left-click cancels. */
            xkill_cancel(priv);
            return GDK_FILTER_REMOVE;
        }
        Window win = xev->xbutton.subwindow;
        if (win != None && win != GDK_ROOT_WINDOW()) {
            win = xkill_toplevel(dpy, win);
            DBG("xkill: killing window 0x%lx\n", win);
            XKillClient(dpy, win);
            XSync(dpy, False);
        }
        xkill_cancel(priv);
        return GDK_FILTER_REMOVE;
    }

    if (xev->type == KeyPress) {
        /* Escape (or any key) cancels. */
        xkill_cancel(priv);
        return GDK_FILTER_REMOVE;
    }

    return GDK_FILTER_CONTINUE;
}

/* ---------------------------------------------------------------------------
 * Enter kill mode
 * ------------------------------------------------------------------------- */

static gboolean
xkill_button_clicked(GtkWidget *widget, GdkEventButton *event,
                     xkill_priv *priv)
{
    GdkWindow *root;
    GdkGrabStatus status;

    ENTER;
    if (event->type != GDK_BUTTON_PRESS || event->button != 1)
        RET(FALSE);

    if (priv->active) {
        xkill_cancel(priv);
        RET(FALSE);
    }

    root = gdk_get_default_root_window();

    /* Create a skull cursor (XC_pirate) on first use. */
    if (!priv->kill_cursor) {
        priv->kill_cursor = XCreateFontCursor(GDK_DISPLAY(), XC_pirate);
    }
    GdkCursor *gdk_cursor = gdk_cursor_new(GDK_PIRATE);

    status = gdk_pointer_grab(root, TRUE,
                              GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK,
                              NULL, gdk_cursor, GDK_CURRENT_TIME);
    gdk_cursor_unref(gdk_cursor);

    if (status != GDK_GRAB_SUCCESS) {
        g_warning("xkill: pointer grab failed");
        RET(FALSE);
    }

    priv->active = TRUE;
    gtk_button_set_label(GTK_BUTTON(priv->button), "...");
    gdk_window_add_filter(root, (GdkFilterFunc) xkill_event_filter, priv);

    DBG("xkill: kill mode active\n");
    RET(TRUE);
}

/* ---------------------------------------------------------------------------
 * Constructor / destructor
 * ------------------------------------------------------------------------- */

static int
xkill_constructor(plugin_instance *p)
{
    xkill_priv *priv;

    ENTER;
    priv = (xkill_priv *) p;

    priv->button = gtk_button_new_with_label("Kill");
    gtk_button_set_relief(GTK_BUTTON(priv->button), GTK_RELIEF_NONE);
    gtk_widget_add_events(priv->button, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(priv->button), "button-press-event",
                     G_CALLBACK(xkill_button_clicked), priv);

    gtk_widget_set_tooltip_markup(p->pwid,
        "<b>Window Killer</b>\n"
        "Click to enter kill mode, then click any window to kill it.\n"
        "Right-click or Escape to cancel.");

    gtk_container_add(GTK_CONTAINER(p->pwid), priv->button);
    gtk_widget_show(priv->button);

    RET(1);
}

static void
xkill_destructor(plugin_instance *p)
{
    xkill_priv *priv = (xkill_priv *) p;

    ENTER;
    if (priv->active)
        xkill_cancel(priv);
    if (priv->kill_cursor) {
        XFreeCursor(GDK_DISPLAY(), priv->kill_cursor);
        priv->kill_cursor = 0;
    }
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "xkill",
    .name        = "Window Killer",
    .version     = "1.0",
    .description = "Click button then click a window to kill its client",
    .priv_size   = sizeof(xkill_priv),
    .constructor = xkill_constructor,
    .destructor  = xkill_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
