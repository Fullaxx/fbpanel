/*
 * windowtitle.c -- fbpanel active window title plugin.
 *
 * Displays the title of the currently focused window in a GtkLabel.
 * The label expands to fill available space and truncates with ellipsis
 * when the text is too wide.
 *
 * When no window has focus (or on a desktop with no windows), the label
 * shows "--".  The plugin never soft-disables: it always loads and
 * handles the case of no active window gracefully.
 *
 * Event source:
 *   Subscribes to the fbev "active_window" GObject signal, which is
 *   triggered by the panel's X event loop whenever _NET_ACTIVE_WINDOW
 *   changes on the root window.
 *
 * Title resolution (same strategy as the taskbar plugin):
 *   1. _NET_WM_NAME (UTF-8, EWMH standard) via get_utf8_property().
 *   2. Falls back to WM_NAME (ICCCM, Latin-1/locale) via
 *      get_textproperty() if _NET_WM_NAME is absent.
 *   3. Shows "--" if the active window is None or the title is unset.
 *
 * Configuration (xconf keys):
 *   MaxWidth -- maximum label width in characters before truncation
 *               (default: 40; 0 means no limit).
 *
 * Widget hierarchy:
 *   p->pwid (GtkBgbox, managed by framework)
 *     priv->label (GtkLabel, expands horizontally, ellipsizes at end)
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

/*
 * windowtitle_priv -- per-instance private state.
 *
 * plugin   -- base class (MUST be first).
 * label    -- GtkLabel showing the active window title.
 * max_width -- max characters to display (0 = no limit).
 */
typedef struct {
    plugin_instance  plugin;
    GtkWidget       *label;
    int              max_width;
} windowtitle_priv;

/*
 * windowtitle_update -- fetch _NET_ACTIVE_WINDOW and refresh the label.
 *
 * Called from the fbev "active_window" signal handler.  Reads
 * _NET_ACTIVE_WINDOW from the root window, then reads _NET_WM_NAME
 * (falling back to WM_NAME) from that window.  Truncates the title
 * to max_width characters if configured.
 *
 * Parameters:
 *   widget -- unused GtkWidget* (fbev signal signature requirement).
 *   priv   -- windowtitle_priv instance.
 */
static void
windowtitle_update(GtkWidget *widget, windowtitle_priv *priv)
{
    Window  *f;
    char    *name = NULL;
    gchar    truncated[256];

    ENTER;
    (void) widget;

    f = get_xaproperty(GDK_ROOT_WINDOW(), a_NET_ACTIVE_WINDOW, XA_WINDOW, 0);
    if (!f || *f == None) {
        if (f)
            XFree(f);
        gtk_label_set_text(GTK_LABEL(priv->label), "--");
        RET();
    }

    /* Try _NET_WM_NAME (UTF-8) first. */
    name = get_utf8_property(*f, a_NET_WM_NAME);

    /* Fall back to legacy WM_NAME (Latin-1 / locale-encoded). */
    if (!name)
        name = get_textproperty(*f, XA_WM_NAME);

    XFree(f);

    if (!name) {
        gtk_label_set_text(GTK_LABEL(priv->label), "--");
        RET();
    }

    DBG("windowtitle: active window title=\"%s\"\n", name);

    if (priv->max_width > 0 && (int) g_utf8_strlen(name, -1) > priv->max_width) {
        /* Truncate at a UTF-8 character boundary. */
        const gchar *end = g_utf8_offset_to_pointer(name, priv->max_width - 1);
        gsize nbytes = (gsize)(end - name);
        if (nbytes >= sizeof(truncated))
            nbytes = sizeof(truncated) - 4;
        memcpy(truncated, name, nbytes);
        /* Append UTF-8 horizontal ellipsis U+2026. */
        memcpy(truncated + nbytes, "\xe2\x80\xa6", 3);
        truncated[nbytes + 3] = '\0';
        gtk_label_set_text(GTK_LABEL(priv->label), truncated);
    } else {
        gtk_label_set_text(GTK_LABEL(priv->label), name);
    }

    g_free(name);
    RET();
}

/*
 * windowtitle_constructor -- initialise the active window title plugin.
 *
 * Creates a GtkLabel, subscribes to fbev "active_window", and sets
 * the initial title by calling windowtitle_update() immediately.
 * This plugin never soft-disables; it always returns 1.
 *
 * Returns: 1 always.
 */
static int
windowtitle_constructor(plugin_instance *p)
{
    windowtitle_priv *priv;

    ENTER;

    priv = (windowtitle_priv *) p;
    priv->max_width = 40;

    XCG(p->xc, "MaxWidth", &priv->max_width, int);

    if (priv->max_width < 0)
        priv->max_width = 0;

    priv->label = gtk_label_new("--");
    gtk_label_set_ellipsize(GTK_LABEL(priv->label), PANGO_ELLIPSIZE_END);

    /* p->pwid is a GtkBin (GtkBgbox); add the label as its single child. */
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);

    /* Connect to the EWMH active-window change signal. */
    g_signal_connect(G_OBJECT(fbev), "active_window",
                     G_CALLBACK(windowtitle_update), priv);

    /* Populate label immediately with the current active window. */
    windowtitle_update(NULL, priv);

    RET(1);
}

/*
 * windowtitle_destructor -- clean up window title plugin resources.
 *
 * Disconnects the fbev "active_window" signal handler.
 * The GtkLabel is destroyed by the framework (p->pwid destruction).
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 */
static void
windowtitle_destructor(plugin_instance *p)
{
    windowtitle_priv *priv = (windowtitle_priv *) p;

    ENTER;
    g_signal_handlers_disconnect_by_func(G_OBJECT(fbev),
                                         windowtitle_update, priv);
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "windowtitle",
    .name        = "Window Title",
    .version     = "1.0",
    .description = "Display the title of the currently focused window",
    .priv_size   = sizeof(windowtitle_priv),
    .constructor = windowtitle_constructor,
    .destructor  = windowtitle_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
