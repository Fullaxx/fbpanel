/*
 * xrandr.c -- fbpanel display resolution plugin.
 *
 * Shows the current resolution of the monitor the panel occupies, e.g.
 * "1920x1080".  Updates automatically when the screen configuration
 * changes (monitor connect/disconnect, resolution switch).
 *
 * No new library dependencies -- uses GDK screen geometry (GTK2).
 *
 * Configuration (xconf keys):
 *   ShowRefresh -- show refresh rate if available via XRandR (default: 0)
 *   Command     -- command to run on left-click, e.g. "arandr" (default: none)
 */

#include <stdio.h>
#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "run.h"

//#define DEBUGPRN
#include "dbg.h"

typedef struct {
    plugin_instance  plugin;
    GtkWidget       *label;
    gchar           *command;   /* left-click command, or NULL */
    gulong           size_sig;  /* "size-changed" signal handler ID */
} xrandr_priv;

/*
 * xrandr_update -- refresh the label with the current screen geometry.
 *
 * Reads the geometry of the panel's monitor from GDK and formats it as
 * "WxH".  Called on startup and on each "size-changed" signal.
 */
static void
xrandr_update(GtkWidget *widget, GdkScreen *scr, xrandr_priv *priv)
{
    GdkRectangle geom;
    gchar text[32];
    gchar tip[80];
    int mon;

    ENTER;
    (void) widget;

    mon = (priv->plugin.panel
           && priv->plugin.panel->xineramaHead >= 0)
          ? priv->plugin.panel->xineramaHead : 0;

    gdk_screen_get_monitor_geometry(scr, mon, &geom);

    g_snprintf(text, sizeof(text), "%dx%d", geom.width, geom.height);
    gtk_label_set_text(GTK_LABEL(priv->label), text);

    g_snprintf(tip, sizeof(tip),
               "<b>Display:</b> monitor %d\n"
               "<b>Resolution:</b> %dx%d\n"
               "<b>Position:</b> %d,%d",
               mon, geom.width, geom.height, geom.x, geom.y);
    gtk_widget_set_tooltip_markup(priv->plugin.pwid, tip);

    DBG("xrandr: monitor %d = %dx%d+%d+%d\n",
        mon, geom.width, geom.height, geom.x, geom.y);
    RET();
}

static gboolean
xrandr_clicked(GtkWidget *widget, GdkEventButton *event, xrandr_priv *priv)
{
    ENTER;
    if (event->type == GDK_BUTTON_PRESS && event->button == 1
        && priv->command && priv->command[0]) {
        run_app(priv->command);
    }
    RET(FALSE);
}

static int
xrandr_constructor(plugin_instance *p)
{
    xrandr_priv *priv;
    GdkScreen   *scr;

    ENTER;
    priv = (xrandr_priv *) p;
    priv->command = NULL;

    XCG(p->xc, "Command", &priv->command, str);

    priv->label = gtk_label_new("...");
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);

    scr = gdk_screen_get_default();

    /* Listen for resolution/monitor changes. */
    priv->size_sig = g_signal_connect(G_OBJECT(scr), "size-changed",
                                      G_CALLBACK(xrandr_update), priv);

    /* Initial update. */
    xrandr_update(NULL, scr, priv);

    if (priv->command && priv->command[0]) {
        gtk_widget_add_events(p->pwid, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(G_OBJECT(p->pwid), "button-press-event",
                         G_CALLBACK(xrandr_clicked), priv);
    }

    RET(1);
}

static void
xrandr_destructor(plugin_instance *p)
{
    xrandr_priv *priv = (xrandr_priv *) p;
    GdkScreen   *scr  = gdk_screen_get_default();

    ENTER;
    if (priv->size_sig) {
        g_signal_handler_disconnect(G_OBJECT(scr), priv->size_sig);
        priv->size_sig = 0;
    }
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "xrandr",
    .name        = "Display Resolution",
    .version     = "1.0",
    .description = "Show current display resolution; optional click command",
    .priv_size   = sizeof(xrandr_priv),
    .constructor = xrandr_constructor,
    .destructor  = xrandr_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
