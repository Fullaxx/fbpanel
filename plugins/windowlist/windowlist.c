/*
 * windowlist.c -- fbpanel window list popup plugin.
 *
 * A small button that, when clicked, pops up a menu listing all open
 * windows.  Selecting a window raises and focuses it via EWMH.
 *
 * This is a compact alternative to the full taskbar plugin; useful on
 * very small panels where a taskbar would be too wide.
 *
 * Data source: _NET_CLIENT_LIST (updated via fbev "client_list" signal).
 * Activation:  sends _NET_ACTIVE_WINDOW client message (same as a pager).
 *
 * No new library dependencies -- X11 and EWMH helpers already linked.
 *
 * Configuration (xconf keys):
 *   MaxTitle -- maximum characters of window title shown per menu item
 *               (default: 50; 0 = no limit).
 */

#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "ewmh.h"

//#define DEBUGPRN
#include "dbg.h"

typedef struct {
    plugin_instance  plugin;
    GtkWidget       *button;
    int              max_title;
} windowlist_priv;

/* ---------------------------------------------------------------------------
 * Window activation
 * ------------------------------------------------------------------------- */

/* Activate (raise + focus) a window using the EWMH _NET_ACTIVE_WINDOW
 * client message.  Source indication 2 = pager/panel. */
static void
windowlist_activate(Window win)
{
    Xclimsg(win, a_NET_ACTIVE_WINDOW,
            2,                           /* source: pager */
            gtk_get_current_event_time(),
            0, 0, 0);
    XSync(GDK_DISPLAY(), False);
}

/* ---------------------------------------------------------------------------
 * Build and show popup menu
 * ------------------------------------------------------------------------- */

/* Called when the user picks a window from the menu. */
static void
wl_item_activate(GtkMenuItem *item, gpointer data)
{
    Window win = (Window)(gulong) data;
    ENTER;
    windowlist_activate(win);
    RET();
}

static void
wl_show_menu(windowlist_priv *priv)
{
    GtkWidget   *menu;
    Window      *wins;
    int          nwins = 0;
    int          i;
    gchar        truncated[256];

    wins = (Window *) get_xaproperty(GDK_ROOT_WINDOW(),
                                     a_NET_CLIENT_LIST,
                                     XA_WINDOW, &nwins);

    menu = gtk_menu_new();

    if (!wins || nwins == 0) {
        GtkWidget *empty = gtk_menu_item_new_with_label("(no windows)");
        gtk_widget_set_sensitive(empty, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), empty);
    } else {
        for (i = 0; i < nwins; i++) {
            gchar     *title;
            GtkWidget *item;
            glong      len;

            /* Skip the panel itself. */
            if (FBPANEL_WIN(wins[i]))
                continue;

            title = get_utf8_property(wins[i], a_NET_WM_NAME);
            if (!title)
                title = get_textproperty(wins[i], XA_WM_NAME);
            if (!title)
                title = g_strdup("(untitled)");

            len = g_utf8_strlen(title, -1);
            if (priv->max_title > 0 && len > priv->max_title) {
                const gchar *end = g_utf8_offset_to_pointer(title,
                                                    priv->max_title - 1);
                gsize nbytes = (gsize)(end - title);
                if (nbytes >= sizeof(truncated) - 4)
                    nbytes = sizeof(truncated) - 4;
                memcpy(truncated, title, nbytes);
                /* UTF-8 ellipsis U+2026 */
                memcpy(truncated + nbytes, "\xe2\x80\xa6", 3);
                truncated[nbytes + 3] = '\0';
                item = gtk_menu_item_new_with_label(truncated);
            } else {
                item = gtk_menu_item_new_with_label(title);
            }
            g_free(title);

            /* Store window ID as gpointer (safe: Window is gulong). */
            g_signal_connect(G_OBJECT(item), "activate",
                             G_CALLBACK(wl_item_activate),
                             (gpointer)(gulong) wins[i]);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
        XFree(wins);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 1,
                   gtk_get_current_event_time());
}

static gboolean
wl_button_clicked(GtkWidget *widget, GdkEventButton *event,
                  windowlist_priv *priv)
{
    ENTER;
    if (event->type == GDK_BUTTON_PRESS && event->button == 1)
        wl_show_menu(priv);
    RET(FALSE);
}

/* ---------------------------------------------------------------------------
 * Constructor / destructor
 * ------------------------------------------------------------------------- */

static int
windowlist_constructor(plugin_instance *p)
{
    windowlist_priv *priv;

    ENTER;
    priv = (windowlist_priv *) p;
    priv->max_title = 50;

    XCG(p->xc, "MaxTitle", &priv->max_title, int);
    if (priv->max_title < 0) priv->max_title = 0;

    priv->button = gtk_button_new_with_label("Win");
    gtk_button_set_relief(GTK_BUTTON(priv->button), GTK_RELIEF_NONE);
    gtk_widget_add_events(priv->button, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(priv->button), "button-press-event",
                     G_CALLBACK(wl_button_clicked), priv);

    gtk_widget_set_tooltip_markup(p->pwid,
        "<b>Window List</b>\n"
        "Click to show all open windows.");

    gtk_container_add(GTK_CONTAINER(p->pwid), priv->button);
    gtk_widget_show(priv->button);

    RET(1);
}

static void
windowlist_destructor(plugin_instance *p)
{
    ENTER;
    (void) p;
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "windowlist",
    .name        = "Window List",
    .version     = "1.0",
    .description = "Popup menu of all open windows",
    .priv_size   = sizeof(windowlist_priv),
    .constructor = windowlist_constructor,
    .destructor  = windowlist_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
