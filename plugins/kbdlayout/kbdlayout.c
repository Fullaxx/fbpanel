/*
 * kbdlayout.c -- fbpanel keyboard layout indicator plugin.
 *
 * Displays the short name of the currently active keyboard layout
 * (e.g. "us", "de", "fr") as a text label.  Left-clicking cycles to the
 * next configured layout; right-clicking shows a menu of all layouts.
 *
 * Uses the XKB extension (part of libX11 -- no new dependency):
 *   XkbGetState()  -- current group index
 *   XkbGetNames()  -- layout short names (symbols)
 *   XkbLockGroup() -- change the active group
 *
 * Polls every 500 ms.  (XKB events could be used instead but require a
 * separate event filter; polling is simpler for a panel plugin.)
 *
 * Configuration (xconf keys):
 *   Period -- poll interval in milliseconds (default: 500).
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

/* Maximum keyboard groups XKB supports. */
#define XKB_MAX_GROUPS 4

typedef struct {
    plugin_instance  plugin;
    GtkWidget       *label;
    guint            timer;
    int              period;
    int              cur_group;      /* last known active group index */
    int              num_groups;     /* total number of configured groups */
    gchar           *group_names[XKB_MAX_GROUPS]; /* short names, heap-alloc'd */
} kbdlayout_priv;

/* ---------------------------------------------------------------------------
 * XKB helpers
 * ------------------------------------------------------------------------- */

/* Fetch group names and count from XKB. Fills priv->group_names[] and
 * priv->num_groups.  Frees any previously loaded names first. */
static void
kbdlayout_load_names(kbdlayout_priv *priv)
{
    XkbDescPtr  desc;
    int         i;
    Display    *dpy = GDK_DISPLAY();

    /* Free old names. */
    for (i = 0; i < XKB_MAX_GROUPS; i++) {
        g_free(priv->group_names[i]);
        priv->group_names[i] = NULL;
    }
    priv->num_groups = 0;

    desc = XkbGetKeyboard(dpy,
                          XkbSymbolsNameMask | XkbGroupNamesMask,
                          XkbUseCoreKbd);
    if (!desc)
        return;

    if (desc->names) {
        /* desc->names->num_sk is not the group count; use desc->ctrls or
         * XkbNumGroups().  Fall back to iterating Atom names. */
        int ngroups = XkbNumGroups(desc->ctrls ? desc->ctrls->num_groups : 0);
        if (ngroups <= 0 || ngroups > XKB_MAX_GROUPS)
            ngroups = XKB_MAX_GROUPS;

        for (i = 0; i < ngroups; i++) {
            Atom name_atom = desc->names->groups[i];
            if (name_atom == None)
                break;
            char *name = XGetAtomName(dpy, name_atom);
            if (name) {
                priv->group_names[i] = g_strdup(name);
                XFree(name);
                priv->num_groups++;
            } else {
                break;
            }
        }
    }

    XkbFreeKeyboard(desc, 0, True);

    /* Fallback: at least one group named "?" */
    if (priv->num_groups == 0) {
        priv->group_names[0] = g_strdup("?");
        priv->num_groups = 1;
    }

    DBG("kbdlayout: %d groups\n", priv->num_groups);
}

/* Return a short (up to 4-char) label for a group name atom string.
 * XKB group names are typically like "English (US)", "German", etc.
 * We extract the first word as the short code, lower-cased. */
static gchar *
kbdlayout_short_name(const gchar *full)
{
    static gchar buf[8];
    int i;
    if (!full || !full[0])
        return g_strdup("?");
    /* Copy first 4 non-space ASCII characters, lower-cased. */
    for (i = 0; full[i] && i < 4; i++) {
        buf[i] = (full[i] >= 'A' && full[i] <= 'Z')
                 ? full[i] + 32 : full[i];
    }
    buf[i] = '\0';
    return g_strdup(buf);
}

/* ---------------------------------------------------------------------------
 * Update
 * ------------------------------------------------------------------------- */

static gboolean
kbdlayout_update(kbdlayout_priv *priv)
{
    XkbStateRec state;
    int group;
    gchar *short_name;
    gchar tip[128];

    ENTER;
    if (XkbGetState(GDK_DISPLAY(), XkbUseCoreKbd, &state) != Success)
        RET(TRUE);

    group = state.group;
    if (group == priv->cur_group)
        RET(TRUE);

    priv->cur_group = group;

    if (group < priv->num_groups && priv->group_names[group]) {
        short_name = kbdlayout_short_name(priv->group_names[group]);
        g_snprintf(tip, sizeof(tip),
                   "<b>Keyboard layout:</b> %s\n"
                   "Group %d of %d\n"
                   "Left-click to cycle; right-click for menu.",
                   priv->group_names[group],
                   group + 1, priv->num_groups);
    } else {
        short_name = g_strdup_printf("%d", group + 1);
        g_snprintf(tip, sizeof(tip),
                   "<b>Keyboard group:</b> %d of %d",
                   group + 1, priv->num_groups);
    }

    gtk_label_set_text(GTK_LABEL(priv->label), short_name);
    gtk_widget_set_tooltip_markup(priv->plugin.pwid, tip);
    g_free(short_name);

    DBG("kbdlayout: group=%d (%s)\n", group,
        (group < priv->num_groups && priv->group_names[group])
         ? priv->group_names[group] : "?");
    RET(TRUE);
}

/* ---------------------------------------------------------------------------
 * Event handlers
 * ------------------------------------------------------------------------- */

/* cycle_to_group -- activate a specific XKB group. */
static void
cycle_to_group(int group)
{
    XkbLockGroup(GDK_DISPLAY(), XkbUseCoreKbd, group);
    XSync(GDK_DISPLAY(), False);
}

static void
kbdlayout_menu_item_activate(GtkMenuItem *item, gpointer data)
{
    int group = (int)(gintptr) data;
    cycle_to_group(group);
}

static void
kbdlayout_show_menu(kbdlayout_priv *priv)
{
    GtkWidget *menu;
    int i;

    menu = gtk_menu_new();
    for (i = 0; i < priv->num_groups; i++) {
        gchar *label = (priv->group_names[i])
                       ? g_strdup(priv->group_names[i])
                       : g_strdup_printf("Group %d", i + 1);
        GtkWidget *item = gtk_menu_item_new_with_label(label);
        g_free(label);
        g_signal_connect(G_OBJECT(item), "activate",
                         G_CALLBACK(kbdlayout_menu_item_activate),
                         (gpointer)(gintptr) i);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 3,
                   gtk_get_current_event_time());
}

static gboolean
kbdlayout_clicked(GtkWidget *widget, GdkEventButton *event,
                  kbdlayout_priv *priv)
{
    ENTER;
    if (event->type != GDK_BUTTON_PRESS)
        RET(FALSE);

    if (event->button == 1) {
        /* Left-click: cycle to next group. */
        int next = (priv->cur_group + 1) % priv->num_groups;
        cycle_to_group(next);
        priv->cur_group = -1; /* force label refresh */
        kbdlayout_update(priv);
    } else if (event->button == 3) {
        /* Right-click: show layout menu. */
        kbdlayout_show_menu(priv);
    }
    RET(TRUE);
}

/* ---------------------------------------------------------------------------
 * Constructor / destructor
 * ------------------------------------------------------------------------- */

static int
kbdlayout_constructor(plugin_instance *p)
{
    kbdlayout_priv *priv;
    XkbStateRec state;

    ENTER;
    priv = (kbdlayout_priv *) p;
    priv->period    = 500;
    priv->cur_group = -1;

    XCG(p->xc, "Period", &priv->period, int);
    if (priv->period < 100) priv->period = 100;

    /* Check that XKB is available. */
    if (XkbGetState(GDK_DISPLAY(), XkbUseCoreKbd, &state) != Success) {
        g_message("kbdlayout: XKB extension not available — plugin disabled");
        RET(0);
    }

    kbdlayout_load_names(priv);

    priv->label = gtk_label_new("...");
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);
    gtk_widget_add_events(p->pwid, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(p->pwid), "button-press-event",
                     G_CALLBACK(kbdlayout_clicked), priv);

    kbdlayout_update(priv);
    priv->timer = g_timeout_add(priv->period,
                                (GSourceFunc) kbdlayout_update, priv);
    RET(1);
}

static void
kbdlayout_destructor(plugin_instance *p)
{
    kbdlayout_priv *priv = (kbdlayout_priv *) p;
    int i;
    ENTER;
    if (priv->timer) {
        g_source_remove(priv->timer);
        priv->timer = 0;
    }
    for (i = 0; i < XKB_MAX_GROUPS; i++) {
        g_free(priv->group_names[i]);
        priv->group_names[i] = NULL;
    }
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "kbdlayout",
    .name        = "Keyboard Layout",
    .version     = "1.0",
    .description = "Show and cycle keyboard layouts via XKB",
    .priv_size   = sizeof(kbdlayout_priv),
    .constructor = kbdlayout_constructor,
    .destructor  = kbdlayout_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
