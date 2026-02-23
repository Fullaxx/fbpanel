/*
 * wincmd.c -- "Show Desktop" button plugin for fbpanel.
 *
 * Provides a single button that, when clicked, either iconifies or shades
 * all windows on the current virtual desktop (configurable via xconf).
 *
 * Configuration keys:
 *   Button1  - action for left button:   "none" | "iconify" | "shade"
 *   Button2  - action for middle button: "none" | "iconify" | "shade"
 *   Icon     - icon name (theme icon)
 *   Image    - image file path (used if Icon is not set)
 *   tooltip  - tooltip markup string
 *
 * Default behaviour:
 *   Button1 = iconify, Button2 = shade
 */

#include <stdlib.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "gtkbgbox.h"
//#define DEBUGPRN
#include "dbg.h"


/*
 * wincmd_priv -- per-instance private state for the wincmd plugin.
 *
 * plugin  - embedded base class (MUST be first member for cast compatibility)
 * button1 - xconf-configured action for left button (WC_ICONIFY default)
 * button2 - xconf-configured action for middle button (WC_SHADE default)
 * action1 - runtime shade-direction toggle for button1 (0=remove, 1=add)
 * action2 - runtime shade-direction toggle for button2 (0=remove, 1=add)
 */
typedef struct {
    plugin_instance plugin;    /* base class -- MUST be first */
    int button1, button2;
    int action1, action2;
} wincmd_priv;

/*
 * Window-command action codes.
 *   WC_NONE    - no action
 *   WC_ICONIFY - iconify / map all windows on current desktop
 *   WC_SHADE   - shade / unshade all windows on current desktop
 */
enum { WC_NONE, WC_ICONIFY, WC_SHADE };


/*
 * wincmd_enum -- xconf_enum table for parsing Button1/Button2 config values.
 *
 * Maps the strings "none", "iconify", "shade" to the WC_* enum constants.
 * Terminated by a {0, NULL} sentinel.
 */
xconf_enum wincmd_enum[] = {
    { .num = WC_NONE,    .str = "none"    },
    { .num = WC_ICONIFY, .str = "iconify" },
    { .num = WC_SHADE,   .str = "shade"   },
    { .num = 0,          .str = NULL      },
};

/*
 * toggle_shaded -- send _NET_WM_STATE_SHADED to all windows on the current desktop.
 *
 * Reads _NET_CLIENT_LIST (not stacking order) from the root window.
 * For each window on the current desktop that is not a dock/desktop/splash:
 *   sends Xclimsg with _NET_WM_STATE_ADD (action=1) or _NET_WM_STATE_REMOVE (action=0).
 *
 * Parameters:
 *   wc     - plugin private state (unused beyond ENTER/RET tracing)
 *   action - 1 to shade, 0 to unshade
 *
 * Note: the commented-out get_xaproperty block for _NET_CURRENT_DESKTOP
 *       has been replaced by the get_net_current_desktop() helper.
 */
static void
toggle_shaded(wincmd_priv *wc, guint32 action)
{
    Window *win = NULL;
    int num, i;
    guint32 tmp2, dno;
    net_wm_window_type nwwt;

    ENTER;
    /* Read the list of managed windows from the root window property.     */
    win = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CLIENT_LIST,
        XA_WINDOW, &num);
    if (!win)
	RET();
    if (!num)
        goto end;

    /* Get the index of the currently active virtual desktop.              */
    //tmp = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP,
    // XA_CARDINAL, 0);
    //dno = *tmp;
    dno = get_net_current_desktop();
    DBG("wincmd: #desk=%d\n", dno);
    //XFree(tmp);

    for (i = 0; i < num; i++) {
        int skip;

        tmp2 = get_net_wm_desktop(win[i]);
        DBG("wincmd: win=0x%x dno=%d...", win[i], tmp2);

        /* Skip windows on other desktops.  tmp2 == (guint32)-1 means the
         * window is sticky (present on all desktops).                     */
        if ((tmp2 != -1) && (tmp2 != dno)) {
            DBG("skip - not cur desk\n");
            continue;
        }

        /* Skip special window types that should not be shaded.            */
        get_net_wm_window_type(win[i], &nwwt);
        skip = (nwwt.dock || nwwt.desktop || nwwt.splash);
        if (skip) {
            DBG("skip - omnipresent window type\n");
            continue;
        }

        /* Send _NET_WM_STATE change request to the window manager.
         * action ? ADD : REMOVE the _NET_WM_STATE_SHADED state atom.     */
        Xclimsg(win[i], a_NET_WM_STATE,
              action ? a_NET_WM_STATE_ADD : a_NET_WM_STATE_REMOVE,
              a_NET_WM_STATE_SHADED, 0, 0, 0);
        DBG("ok\n");
    }

end:
    XFree(win);
    RET();
}

/*
 * toggle_iconify -- iconify or map all windows on the current desktop.
 *
 * Reads _NET_CLIENT_LIST_STACKING (stacking order, bottom-to-top) from
 * the root window.  Filters out other-desktop and special-type windows.
 *
 * Decision logic:
 *   raise = TRUE  if ALL remaining windows are currently hidden or shaded.
 *   raise = FALSE if ANY window is visible (not hidden, not shaded).
 *
 * If raise is TRUE:  XMapWindow all filtered windows (restore them).
 * If raise is FALSE: XIconifyWindow all filtered windows.
 *
 * Windows are processed in reverse stacking order (top-to-bottom) during
 * iconify, and the same order during restore.
 *
 * Note: "if all windows are iconified then open all,
 *         if any are open then iconify 'em"
 */
static void
toggle_iconify(wincmd_priv *wc)
{
    Window *win, *awin;
    int num, i, j, dno, raise;
    guint32 tmp;
    net_wm_window_type nwwt;
    net_wm_state nws;

    ENTER;
    /* Read windows in stacking order (needed for correct raise/lower).   */
    win = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CLIENT_LIST_STACKING,
            XA_WINDOW, &num);
    if (!win)
	RET();
    if (!num)
        goto end;

    /* Build filtered array of windows on the current desktop.             */
    awin = g_new(Window, num);
    dno = get_net_current_desktop();
    raise = 1;    /* assume all windows are iconified/shaded until proven otherwise */

    for (j = 0, i = 0; i < num; i++) {
        tmp = get_net_wm_desktop(win[i]);
        DBG("wincmd: win=0x%x dno=%d...", win[i], tmp);

        /* Skip windows assigned to other desktops.                        */
        if ((tmp != -1) && (tmp != dno))
            continue;

        /* Skip dock, desktop, and splash windows.                         */
        get_net_wm_window_type(win[i], &nwwt);
        tmp = (nwwt.dock || nwwt.desktop || nwwt.splash);
        if (tmp)
            continue;

        /* Update raise flag: if any window is visible (not hidden or
         * shaded), we should iconify rather than restore.                 */
        get_net_wm_state(win[i], &nws);
        raise = raise && (nws.hidden || nws.shaded);;   /* note double semicolon */
        awin[j++] = win[i];
    }

    /* Process the filtered window list in reverse order (j..0).          */
    while (j-- > 0) {
        if (raise)
            /* All were iconified/shaded: map (restore) them.             */
            XMapWindow (GDK_DISPLAY(), awin[j]);
        else
            /* At least one was visible: iconify them all.                */
            XIconifyWindow(GDK_DISPLAY(), awin[j], DefaultScreen(GDK_DISPLAY()));
    }

    g_free(awin);
end:
    XFree(win);
    RET();
}

/*
 * do_action -- execute a WC_* action, toggling shade state as needed.
 */
static void
do_action(wincmd_priv *wc, int action, int *action_toggle)
{
    switch (action) {
    case WC_ICONIFY:
        toggle_iconify(wc);
        break;
    case WC_SHADE:
        *action_toggle = 1 - *action_toggle;
        toggle_shaded(wc, *action_toggle);
        break;
    case WC_NONE:
    default:
        break;
    }
}

/*
 * clicked -- button-press-event handler for the wincmd button.
 *
 * Dispatches to do_action() based on wc->button1 / wc->button2 so that
 * the xconf Button1/Button2 config values are honoured.
 */
static gint
clicked (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    wincmd_priv *wc = (wincmd_priv *) data;

    ENTER;
    if (event->type != GDK_BUTTON_PRESS)
        RET(FALSE);

    if (event->button == 1)
        do_action(wc, wc->button1, &wc->action1);
    else if (event->button == 2)
        do_action(wc, wc->button2, &wc->action2);
    else
        DBG("wincmd: unsupported button %d\n", event->button);

    RET(FALSE);
}

/*
 * wincmd_destructor -- release wincmd-specific resources.
 *
 * Nothing to free: all widgets are children of p->pwid and are
 * destroyed automatically by the plugin framework.
 */
static void
wincmd_destructor(plugin_instance *p)
{
    ENTER;
    (void)p;
    RET();
}

/*
 * wincmd_constructor -- initialise the wincmd plugin instance.
 *
 * Steps:
 *  1. Set default button actions: button1=WC_ICONIFY, button2=WC_SHADE.
 *  2. Parse xconf keys: Button1, Button2, Icon, Image, tooltip.
 *     - iname (Icon) is a non-owning XCG str pointer; do NOT g_free.
 *     - fname (Image) is XCG str then expand_tilda'd; MUST g_free after use.
 *     - tooltip is a non-owning XCG str pointer.
 *  3. Determine button size from panel orientation.
 *  4. Create the button via fb_button_new().
 *  5. Connect button_press_event → clicked().
 *  6. Apply transparent background if the panel uses pseudo-transparency.
 *  7. Set tooltip markup if provided.
 *  8. Free the expand_tilda'd fname copy.
 *
 * Returns 1 on success.
 */
static int
wincmd_constructor(plugin_instance *p)
{
    gchar *tooltip, *fname, *iname;
    wincmd_priv *wc;
    GtkWidget *button;
    int w, h;

    ENTER;
    wc = (wincmd_priv *) p;
    tooltip = fname = iname = NULL;

    /* Default actions (may be overridden by xconf below).                 */
    wc->button1 = WC_ICONIFY;
    wc->button2 = WC_SHADE;

    /* Parse configuration.  button1/button2 are dispatched by clicked()
     * via do_action().                                                     */
    XCG(p->xc, "Button1", &wc->button1, enum, wincmd_enum);
    XCG(p->xc, "Button2", &wc->button2, enum, wincmd_enum);
    XCG(p->xc, "Icon",    &iname,        str);   /* non-owning pointer    */
    XCG(p->xc, "Image",   &fname,        str);   /* non-owning pointer    */
    XCG(p->xc, "tooltip", &tooltip,      str);   /* non-owning pointer    */

    /* expand_tilda returns a g_strdup'd copy; remember to g_free below.   */
    fname = expand_tilda(fname);

    /* Size the button to fill the panel cross-axis.                       */
    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        w = -1;
        h = p->panel->max_elem_height;
    } else {
        w = p->panel->max_elem_height;
        h = -1;
    }

    /* Create the icon button; 0x202020 is the dark background blend color.*/
    button = fb_button_new(iname, fname, w, h, 0x202020, NULL);
    gtk_container_set_border_width(GTK_CONTAINER(button), 0);
    g_signal_connect(G_OBJECT(button), "button_press_event",
          G_CALLBACK(clicked), (gpointer)wc);

    gtk_widget_show(button);
    gtk_container_add(GTK_CONTAINER(p->pwid), button);

    /* Apply the panel's pseudo-transparency tint to the button background.*/
    if (p->panel->transparent)
        gtk_bgbox_set_background(button, BG_INHERIT,
            p->panel->tintcolor, p->panel->alpha);

    /* Free the expand_tilda'd copy of the image path.                     */
    g_free(fname);
    /* iname and tooltip are non-owning XCG str pointers: do NOT g_free.   */

    if (tooltip)
        gtk_widget_set_tooltip_markup(button, tooltip);

    RET(1);
}


/*
 * plugin_class descriptor -- exported symbol used by the plugin loader.
 *
 * priv_size = sizeof(wincmd_priv) tells the loader how many bytes to
 * allocate for the combined plugin_instance + wincmd_priv struct.
 */
static plugin_class class = {
    .count       = 0,
    .type        = "wincmd",
    .name        = "Show desktop",
    .version     = "1.0",
    .description = "Show Desktop button",
    .priv_size   = sizeof(wincmd_priv),


    .constructor = wincmd_constructor,
    .destructor = wincmd_destructor,
};

/* class_ptr is the symbol the plugin loader looks up via dlsym().        */
static plugin_class *class_ptr = (plugin_class *) &class;
