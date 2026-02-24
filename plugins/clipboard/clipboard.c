/*
 * clipboard.c -- fbpanel clipboard history plugin.
 *
 * Monitors the X11 CLIPBOARD selection via GTK2's GtkClipboard.  When
 * the clipboard owner changes and the new content is text, it is prepended
 * to a ring buffer of up to MaxHistory entries.  Left-clicking the panel
 * button pops up a GtkMenu listing the history; clicking an item restores
 * it as the current clipboard content.
 *
 * PRIMARY selection (mouse-highlight paste) is also monitored optionally.
 *
 * No new library dependencies -- GTK2/GDK already linked.
 *
 * Configuration (xconf keys):
 *   MaxHistory   -- maximum number of entries to remember (default: 10).
 *   WatchPrimary -- also watch the PRIMARY selection (default: 0).
 */

#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/* Maximum label length shown in the popup menu per entry. */
#define CLIP_MENU_MAX_CHARS 60

typedef struct {
    plugin_instance  plugin;
    GtkWidget       *button;
    GtkClipboard    *clip_cb;   /* CLIPBOARD selection */
    GtkClipboard    *clip_pri;  /* PRIMARY selection (optional) */
    gulong           cb_sig;    /* "owner-change" handler for CLIPBOARD */
    gulong           pri_sig;   /* "owner-change" handler for PRIMARY */
    GList           *history;   /* GList of g_strdup'd strings (newest first) */
    int              max_hist;
    gboolean         watch_primary;
    gboolean         receiving;  /* guard against recursive owner-change */
} clipboard_priv;

/* ---------------------------------------------------------------------------
 * History management
 * ------------------------------------------------------------------------- */

static void
clip_history_prepend(clipboard_priv *priv, const gchar *text)
{
    GList *l;

    if (!text || !text[0])
        return;

    /* Skip if identical to the most recent entry. */
    if (priv->history &&
        strcmp((gchar *) priv->history->data, text) == 0)
        return;

    /* Remove existing duplicate lower in the list. */
    for (l = priv->history; l; l = l->next) {
        if (strcmp((gchar *) l->data, text) == 0) {
            g_free(l->data);
            priv->history = g_list_delete_link(priv->history, l);
            break;
        }
    }

    priv->history = g_list_prepend(priv->history, g_strdup(text));

    /* Trim to MaxHistory. */
    while (g_list_length(priv->history) > (guint) priv->max_hist) {
        GList *last = g_list_last(priv->history);
        g_free(last->data);
        priv->history = g_list_delete_link(priv->history, last);
    }
}

static void
clip_history_free(clipboard_priv *priv)
{
    GList *l;
    for (l = priv->history; l; l = l->next)
        g_free(l->data);
    g_list_free(priv->history);
    priv->history = NULL;
}

/* ---------------------------------------------------------------------------
 * Clipboard callbacks
 * ------------------------------------------------------------------------- */

/* Called asynchronously by GTK once the clipboard text is available. */
static void
clip_text_received(GtkClipboard *clipboard, const gchar *text, gpointer data)
{
    clipboard_priv *priv = (clipboard_priv *) data;

    if (text && text[0])
        clip_history_prepend(priv, text);

    priv->receiving = FALSE;
}

/* "owner-change" fires when the clipboard selection changes. */
static void
clip_owner_changed(GtkClipboard *clipboard,
                   GdkEvent     *event,
                   gpointer      data)
{
    clipboard_priv *priv = (clipboard_priv *) data;

    ENTER;
    if (priv->receiving)
        RET();

    priv->receiving = TRUE;
    gtk_clipboard_request_text(clipboard, clip_text_received, priv);
    RET();
}

/* ---------------------------------------------------------------------------
 * Popup menu
 * ------------------------------------------------------------------------- */

/* Called when the user picks a history item: restore it to CLIPBOARD. */
static void
clip_item_activate(GtkMenuItem *item, gpointer data)
{
    clipboard_priv *priv = (clipboard_priv *) data;
    const gchar *text;

    ENTER;
    text = (const gchar *) g_object_get_data(G_OBJECT(item), "clip_text");
    if (!text)
        RET();

    /* Block our own owner-change handler while we set the clipboard. */
    if (priv->cb_sig)
        g_signal_handler_block(G_OBJECT(priv->clip_cb), priv->cb_sig);
    gtk_clipboard_set_text(priv->clip_cb, text, -1);
    if (priv->cb_sig)
        g_signal_handler_unblock(G_OBJECT(priv->clip_cb), priv->cb_sig);

    /* Move this entry to the top of the history. */
    clip_history_prepend(priv, text);
    RET();
}

static void
clip_clear_activate(GtkMenuItem *item, gpointer data)
{
    clipboard_priv *priv = (clipboard_priv *) data;
    clip_history_free(priv);
}

static void
clip_show_menu(clipboard_priv *priv)
{
    GtkWidget *menu;
    GList     *l;
    gchar      truncated[CLIP_MENU_MAX_CHARS * 4 + 4]; /* UTF-8 headroom */
    gboolean   any = FALSE;

    menu = gtk_menu_new();

    for (l = priv->history; l; l = l->next) {
        GtkWidget *item;
        gchar     *text = (gchar *) l->data;
        glong      len  = g_utf8_strlen(text, -1);

        if (len > CLIP_MENU_MAX_CHARS) {
            const gchar *end = g_utf8_offset_to_pointer(text,
                                                        CLIP_MENU_MAX_CHARS - 1);
            gsize nbytes = (gsize)(end - text);
            if (nbytes >= sizeof(truncated) - 4)
                nbytes = sizeof(truncated) - 4;
            memcpy(truncated, text, nbytes);
            /* UTF-8 ellipsis U+2026 */
            memcpy(truncated + nbytes, "\xe2\x80\xa6", 3);
            truncated[nbytes + 3] = '\0';
            item = gtk_menu_item_new_with_label(truncated);
        } else {
            item = gtk_menu_item_new_with_label(text);
        }

        g_object_set_data(G_OBJECT(item), "clip_text", text);
        g_signal_connect(G_OBJECT(item), "activate",
                         G_CALLBACK(clip_item_activate), priv);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        any = TRUE;
    }

    if (!any) {
        GtkWidget *empty = gtk_menu_item_new_with_label("(empty)");
        gtk_widget_set_sensitive(empty, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), empty);
    } else {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        GtkWidget *clear = gtk_menu_item_new_with_label("Clear history");
        g_signal_connect(G_OBJECT(clear), "activate",
                         G_CALLBACK(clip_clear_activate), priv);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), clear);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 1,
                   gtk_get_current_event_time());
}

static gboolean
clip_button_clicked(GtkWidget *widget, GdkEventButton *event,
                    clipboard_priv *priv)
{
    ENTER;
    if (event->type == GDK_BUTTON_PRESS && event->button == 1)
        clip_show_menu(priv);
    RET(FALSE);
}

/* ---------------------------------------------------------------------------
 * Constructor / destructor
 * ------------------------------------------------------------------------- */

static int
clipboard_constructor(plugin_instance *p)
{
    clipboard_priv *priv;

    ENTER;
    priv = (clipboard_priv *) p;
    priv->max_hist      = 10;
    priv->watch_primary = FALSE;

    XCG(p->xc, "MaxHistory",   &priv->max_hist,      int);
    XCG(p->xc, "WatchPrimary", &priv->watch_primary, int);

    if (priv->max_hist < 1)  priv->max_hist = 1;
    if (priv->max_hist > 100) priv->max_hist = 100;

    priv->button = gtk_button_new_with_label("Clip");
    gtk_button_set_relief(GTK_BUTTON(priv->button), GTK_RELIEF_NONE);
    gtk_widget_add_events(priv->button, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(priv->button), "button-press-event",
                     G_CALLBACK(clip_button_clicked), priv);

    gtk_container_add(GTK_CONTAINER(p->pwid), priv->button);
    gtk_widget_show(priv->button);

    gtk_widget_set_tooltip_markup(p->pwid,
        "<b>Clipboard History</b>\n"
        "Click to show recent clipboard entries.");

    /* Connect to CLIPBOARD selection. */
    priv->clip_cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    priv->cb_sig  = g_signal_connect(G_OBJECT(priv->clip_cb),
                                     "owner-change",
                                     G_CALLBACK(clip_owner_changed), priv);

    /* Optionally watch PRIMARY. */
    if (priv->watch_primary) {
        priv->clip_pri = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
        priv->pri_sig  = g_signal_connect(G_OBJECT(priv->clip_pri),
                                          "owner-change",
                                          G_CALLBACK(clip_owner_changed), priv);
    }

    /* Seed history with whatever is currently on the clipboard. */
    priv->receiving = TRUE;
    gtk_clipboard_request_text(priv->clip_cb, clip_text_received, priv);

    RET(1);
}

static void
clipboard_destructor(plugin_instance *p)
{
    clipboard_priv *priv = (clipboard_priv *) p;
    ENTER;
    if (priv->cb_sig) {
        g_signal_handler_disconnect(G_OBJECT(priv->clip_cb), priv->cb_sig);
        priv->cb_sig = 0;
    }
    if (priv->pri_sig) {
        g_signal_handler_disconnect(G_OBJECT(priv->clip_pri), priv->pri_sig);
        priv->pri_sig = 0;
    }
    clip_history_free(priv);
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "clipboard",
    .name        = "Clipboard",
    .version     = "1.0",
    .description = "Clipboard history popup (monitors CLIPBOARD selection)",
    .priv_size   = sizeof(clipboard_priv),
    .constructor = clipboard_constructor,
    .destructor  = clipboard_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
