/*
 * capslock.c -- fbpanel keyboard lock indicator plugin.
 *
 * Shows the state of Caps Lock, Num Lock, and/or Scroll Lock as coloured
 * text labels in the panel.  Active indicators are shown in a configurable
 * "active" colour; inactive ones in the "inactive" colour (or hidden).
 *
 * Uses XkbGetIndicatorState() (part of libX11, no new dependency).
 * Polls every 200 ms for near-zero-latency indication.
 *
 * Configuration (xconf keys):
 *   ShowCaps   -- show Caps Lock indicator (default: 1)
 *   ShowNum    -- show Num Lock indicator (default: 1)
 *   ShowScroll -- show Scroll Lock indicator (default: 0)
 *   HideInactive -- hide inactive indicators entirely (default: 0)
 */

#include <string.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <gdk/gdkx.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/* XKB indicator bit positions (standardised). */
#define XKB_CAPS_LOCK_BIT   (1u << 0)
#define XKB_NUM_LOCK_BIT    (1u << 1)
#define XKB_SCROLL_LOCK_BIT (1u << 2)

typedef struct {
    plugin_instance  plugin;
    GtkWidget       *box;          /* GtkHBox holding the indicator labels */
    GtkWidget       *caps_label;
    GtkWidget       *num_label;
    GtkWidget       *scroll_label;
    guint            timer;
    gboolean         show_caps;
    gboolean         show_num;
    gboolean         show_scroll;
    gboolean         hide_inactive;
    unsigned int     last_state;   /* previous indicator bitmask */
} capslock_priv;

/* ---------------------------------------------------------------------------
 * Update
 * ------------------------------------------------------------------------- */

static void
capslock_apply_label(GtkWidget *label, gboolean active,
                     gboolean hide_inactive)
{
    if (!label)
        return;
    if (hide_inactive && !active) {
        gtk_widget_hide(label);
        return;
    }
    gtk_widget_show(label);
    if (active)
        /* Bold, brighter text for active lock. */
        gtk_widget_modify_fg(label, GTK_STATE_NORMAL, NULL); /* theme default */
    else {
        /* Dim text for inactive lock. */
        GdkColor dim = { 0, 0x8000, 0x8000, 0x8000 };
        gtk_widget_modify_fg(label, GTK_STATE_NORMAL, &dim);
    }
}

static gboolean
capslock_update(capslock_priv *priv)
{
    unsigned int state = 0;

    ENTER;
    XkbGetIndicatorState(GDK_DISPLAY(), XkbUseCoreKbd, &state);

    if (state == priv->last_state)
        RET(TRUE);

    priv->last_state = state;

    if (priv->show_caps && priv->caps_label) {
        gboolean on = (state & XKB_CAPS_LOCK_BIT) != 0;
        const gchar *markup = on ? "<b>A</b>" : "a";
        gtk_label_set_markup(GTK_LABEL(priv->caps_label), markup);
        capslock_apply_label(priv->caps_label, on, priv->hide_inactive);
    }
    if (priv->show_num && priv->num_label) {
        gboolean on = (state & XKB_NUM_LOCK_BIT) != 0;
        const gchar *markup = on ? "<b>1</b>" : "1";
        gtk_label_set_markup(GTK_LABEL(priv->num_label), markup);
        capslock_apply_label(priv->num_label, on, priv->hide_inactive);
    }
    if (priv->show_scroll && priv->scroll_label) {
        gboolean on = (state & XKB_SCROLL_LOCK_BIT) != 0;
        const gchar *markup = on ? "<b>S</b>" : "s";
        gtk_label_set_markup(GTK_LABEL(priv->scroll_label), markup);
        capslock_apply_label(priv->scroll_label, on, priv->hide_inactive);
    }

    DBG("capslock: state=0x%x\n", state);
    RET(TRUE);
}

/* ---------------------------------------------------------------------------
 * Constructor / destructor
 * ------------------------------------------------------------------------- */

static int
capslock_constructor(plugin_instance *p)
{
    capslock_priv *priv;

    ENTER;
    priv = (capslock_priv *) p;
    priv->show_caps     = TRUE;
    priv->show_num      = TRUE;
    priv->show_scroll   = FALSE;
    priv->hide_inactive = FALSE;
    priv->last_state    = 0xFFFFFFFF; /* force first update */

    XCG(p->xc, "ShowCaps",      &priv->show_caps,     int);
    XCG(p->xc, "ShowNum",       &priv->show_num,      int);
    XCG(p->xc, "ShowScroll",    &priv->show_scroll,   int);
    XCG(p->xc, "HideInactive",  &priv->hide_inactive, int);

    /* Box to hold indicators side by side. */
    priv->box = gtk_hbox_new(FALSE, 2);
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->box);

    if (priv->show_caps) {
        priv->caps_label = gtk_label_new("a");
        gtk_box_pack_start(GTK_BOX(priv->box), priv->caps_label,
                           FALSE, FALSE, 1);
    }
    if (priv->show_num) {
        priv->num_label = gtk_label_new("1");
        gtk_box_pack_start(GTK_BOX(priv->box), priv->num_label,
                           FALSE, FALSE, 1);
    }
    if (priv->show_scroll) {
        priv->scroll_label = gtk_label_new("s");
        gtk_box_pack_start(GTK_BOX(priv->box), priv->scroll_label,
                           FALSE, FALSE, 1);
    }

    gtk_widget_show_all(priv->box);

    capslock_update(priv);
    priv->timer = g_timeout_add(200, (GSourceFunc) capslock_update, priv);

    gtk_widget_set_tooltip_markup(p->pwid,
        "<b>Lock Keys</b>: Caps / Num / Scroll");

    RET(1);
}

static void
capslock_destructor(plugin_instance *p)
{
    capslock_priv *priv = (capslock_priv *) p;
    ENTER;
    if (priv->timer) {
        g_source_remove(priv->timer);
        priv->timer = 0;
    }
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "capslock",
    .name        = "Lock Keys",
    .version     = "1.0",
    .description = "Caps Lock / Num Lock / Scroll Lock indicator",
    .priv_size   = sizeof(capslock_priv),
    .constructor = capslock_constructor,
    .destructor  = capslock_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
