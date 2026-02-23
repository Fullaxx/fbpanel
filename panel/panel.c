/*
 * panel.c -- fbpanel main entry point and panel lifecycle.
 *
 * This file implements:
 *   1. main() — GTK init, argument parsing, profile loading, and the
 *      restart loop (gtk_main → panel_stop → repeat until force_quit != 0).
 *   2. panel_start_gui() — builds the complete GTK widget hierarchy:
 *         GtkWindow (topgwin)
 *           └─ GtkBgbox (bbox)
 *                └─ GtkHBox/VBox (lbox)
 *                     └─ GtkHBox/VBox (box) — plugins are packed here
 *   3. panel_event_filter() — GDK root-window PropertyNotify handler that
 *      translates X11 atom changes into FbEv signals.
 *   4. Autohide state machine (VISIBLE → WAITING → HIDDEN → VISIBLE).
 *   5. Config parsing (panel_parse_global, panel_parse_plugin).
 *   6. WM strut management (panel_set_wm_strut).
 *
 * Global state (single-panel design):
 *   p / the_panel  — the single active panel instance.
 *   fbev           — the FbEv event-bus singleton.
 *   force_quit     — 0 = restart after gtk_main() returns, 1 = exit.
 *   config         — 1 if --configure was passed on the command line.
 *   log_level      — verbosity for DBG()/ERR() macros.
 *   xineramaHead   — Xinerama head index (-1 = no preference).
 *   mwid, hpid     — GLib timeout source IDs for autohide mouse-watcher
 *                    and hide-panel timer.  These are globals, which limits
 *                    fbpanel to a single panel per process.
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "plugin.h"
#include "panel.h"
#include "misc.h"
#include "bg.h"
#include "gtkbgbox.h"


static gchar version[] = PROJECT_VERSION;
static gchar *profile = "default";     /* name of the active profile */
static gchar *profile_file;            /* full path to the profile config file */

guint mwid; /* GLib source ID for the autohide mouse-watcher timer (global) */
guint hpid; /* GLib source ID for the autohide hide-panel timer (global) */


FbEv *fbev;          /* FbEv singleton; created in panel_start(), destroyed in panel_stop() */
gint force_quit = 0; /* 0 = restart after gtk_main() returns; non-zero = exit process */
int config;          /* 1 if --configure / -C flag was given */
int xineramaHead = FBPANEL_INVALID_XINERAMA_HEAD; /* Xinerama screen index; -1 = auto */

//#define DEBUGPRN
#include "dbg.h"

/** verbosity level of dbg and log functions */
int log_level = LOG_WARN;

static panel *p;    /* the active panel (used by panel_parse_* helpers) */
panel *the_panel;   /* same pointer, exposed to plugins via panel.h */

/*
 * panel_set_wm_strut -- set _NET_WM_STRUT and _NET_WM_STRUT_PARTIAL on topxwin.
 *
 * Reserves screen space along the panel's edge so maximised windows don't
 * overlap the panel.  Called from panel_configure_event() once the panel
 * reaches its final position, and again from panel_start_gui() immediately
 * after the widget tree is shown.
 *
 * Does nothing if:
 *   - the panel window is not yet mapped (WMs ignore struts on unmapped windows).
 *   - autohide is enabled (the panel moves off-screen; struts would block apps).
 *
 * Note: data[4..11] encode the start/end of the strut along the screen axis.
 */
void
panel_set_wm_strut(panel *p)
{
    gulong data[12] = { 0 };
    int i = 4;

    ENTER;
    if (!GTK_WIDGET_MAPPED(p->topgwin))
        return;
    /* most wm's tend to ignore struts of unmapped windows, and that's how
     * fbpanel hides itself. so no reason to set it. */
    if (p->autohide)
        return;
    switch (p->edge) {
    case EDGE_LEFT:
        i = 0;
        data[i] = p->aw + p->ymargin;
        data[4 + i*2] = p->ay;
        data[5 + i*2] = p->ay + p->ah;
        if (p->autohide) data[i] = p->height_when_hidden;
        break;
    case EDGE_RIGHT:
        i = 1;
        data[i] = p->aw + p->ymargin;
        data[4 + i*2] = p->ay;
        data[5 + i*2] = p->ay + p->ah;
        if (p->autohide) data[i] = p->height_when_hidden;
        break;
    case EDGE_TOP:
        i = 2;
        data[i] = p->ah + p->ymargin;
        data[4 + i*2] = p->ax;
        data[5 + i*2] = p->ax + p->aw;
        if (p->autohide) data[i] = p->height_when_hidden;
        break;
    case EDGE_BOTTOM:
        i = 3;
        data[i] = p->ah + p->ymargin;
        data[4 + i*2] = p->ax;
        data[5 + i*2] = p->ax + p->aw;
        if (p->autohide) data[i] = p->height_when_hidden;
        break;
    default:
        ERR("wrong edge %d. strut won't be set\n", p->edge);
        RET();
    }
    DBG("type %d. width %ld. from %ld to %ld\n", i, data[i], data[4 + i*2],
          data[5 + i*2]);

    /* if wm supports STRUT_PARTIAL it will ignore STRUT */
    XChangeProperty(GDK_DISPLAY(), p->topxwin, a_NET_WM_STRUT_PARTIAL,
        XA_CARDINAL, 32, PropModeReplace,  (unsigned char *) data, 12);
    /* old spec, for wms that do not support STRUT_PARTIAL */
    XChangeProperty(GDK_DISPLAY(), p->topxwin, a_NET_WM_STRUT,
        XA_CARDINAL, 32, PropModeReplace,  (unsigned char *) data, 4);

    RET();
}
#if 0
static void
print_wmdata(panel *p)
{
    int i;

    ENTER;
    RET();
    DBG("desktop %d/%d\n", p->curdesk, p->desknum);
    DBG("workarea\n");
    for (i = 0; i < p->wa_len/4; i++)
        DBG("(%d, %d) x (%d, %d)\n",
              p->workarea[4*i + 0],
              p->workarea[4*i + 1],
              p->workarea[4*i + 2],
              p->workarea[4*i + 3]);
    RET();
}
#endif

/*
 * panel_event_filter -- GDK root-window event filter.
 *
 * Installed with gdk_window_add_filter() in panel_start_gui().
 * Called for every GDK event; most are passed through unchanged.
 *
 * Only handles PropertyNotify events on the root window.
 * Translates _NET_* atom changes into FbEv signals (via fb_ev_trigger).
 *
 * Special cases:
 *   _XROOTPMAP_ID          — refreshes background (if transparent).
 *   _NET_DESKTOP_GEOMETRY  — calls gtk_main_quit() to trigger a restart.
 *
 * BUG: When _NET_DESKTOP_GEOMETRY fires, gtk_main_quit() is called but
 *   force_quit is NOT set to 0 (RESTART).  The restart loop condition is
 *   `while (force_quit == 0)`, so after quitting, force_quit is still 0
 *   and the panel restarts.  This actually works, but it is fragile and
 *   relies on force_quit's initial value of 0.
 */
static GdkFilterReturn
panel_event_filter(GdkXEvent *xevent, GdkEvent *event, panel *p)
{
    Atom at;
    Window win;
    XEvent *ev = (XEvent *) xevent;

    ENTER;
    DBG("win = 0x%lx\n", ev->xproperty.window);
    if (ev->type != PropertyNotify )
        RET(GDK_FILTER_CONTINUE);   /* ignore all non-property events */

    at = ev->xproperty.atom;
    win = ev->xproperty.window;
    DBG("win=%lx at=%ld\n", win, at);
    if (win == GDK_ROOT_WINDOW()) {
        if (at == a_NET_CLIENT_LIST) {
            DBG("A_NET_CLIENT_LIST\n");
            fb_ev_trigger(fbev, EV_CLIENT_LIST);
        } else if (at == a_NET_CURRENT_DESKTOP) {
            DBG("A_NET_CURRENT_DESKTOP\n");
            p->curdesk = get_net_current_desktop();
            fb_ev_trigger(fbev, EV_CURRENT_DESKTOP);
        } else if (at == a_NET_NUMBER_OF_DESKTOPS) {
            DBG("A_NET_NUMBER_OF_DESKTOPS\n");
            p->desknum = get_net_number_of_desktops();
            fb_ev_trigger(fbev, EV_NUMBER_OF_DESKTOPS);
        } else if (at == a_NET_DESKTOP_NAMES) {
            DBG("A_NET_DESKTOP_NAMES\n");
            fb_ev_trigger(fbev, EV_DESKTOP_NAMES);
        } else if (at == a_NET_ACTIVE_WINDOW) {
            DBG("A_NET_ACTIVE_WINDOW\n");
            fb_ev_trigger(fbev, EV_ACTIVE_WINDOW);
        }else if (at == a_NET_CLIENT_LIST_STACKING) {
            DBG("A_NET_CLIENT_LIST_STACKING\n");
            fb_ev_trigger(fbev, EV_CLIENT_LIST_STACKING);
        } else if (at == a_NET_WORKAREA) {
            DBG("A_NET_WORKAREA\n");
            //p->workarea = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_WORKAREA,
            //      XA_CARDINAL, &p->wa_len);
            //print_wmdata(p);
        } else if (at == a_XROOTPMAP_ID) {
            /* root window pixmap changed → refresh pseudo-transparent background */
            if (p->transparent)
                fb_bg_notify_changed_bg(p->bg);
        } else if (at == a_NET_DESKTOP_GEOMETRY) {
            DBG("a_NET_DESKTOP_GEOMETRY\n");
            /* desktop geometry changed → force a panel restart */
            gtk_main_quit();
        } else
            RET(GDK_FILTER_CONTINUE);
        RET(GDK_FILTER_REMOVE);    /* event handled; swallow it */
    }
    DBG("non root %lx\n", win);
    RET(GDK_FILTER_CONTINUE);
}

/****************************************************
 *         panel's handlers for GTK events          *
 ****************************************************/

/*
 * panel_destroy_event -- topgwin "destroy-event" handler.
 *
 * Sets force_quit=1 so the restart loop in main() exits cleanly.
 */
static gint
panel_destroy_event(GtkWidget * widget, GdkEvent * event, gpointer data)
{
    ENTER;
    gtk_main_quit();
    force_quit = 1;
    RET(FALSE);
}

/*
 * panel_size_req -- topgwin "size-request" handler.
 *
 * If widthtype/heighttype is WIDTH_REQUEST or HEIGHT_REQUEST, updates
 * p->width/p->height from the requisition, then recalculates the final
 * panel geometry and overrides the requisition with the computed values.
 */
static void
panel_size_req(GtkWidget *widget, GtkRequisition *req, panel *p)
{
    ENTER;
    DBG("IN req=(%d, %d)\n", req->width, req->height);
    if (p->widthtype == WIDTH_REQUEST)
        p->width = (p->orientation == GTK_ORIENTATION_HORIZONTAL) ? req->width : req->height;
    if (p->heighttype == HEIGHT_REQUEST)
        p->height = (p->orientation == GTK_ORIENTATION_HORIZONTAL) ? req->height : req->width;
    calculate_position(p);
    req->width  = p->aw;
    req->height = p->ah;
    DBG("OUT req=(%d, %d)\n", req->width, req->height);
    RET();
}


/*
 * panel_size_alloc -- topgwin "size-allocate" handler (debug/trace only).
 *
 * Does nothing meaningful; logs the allocation for debugging.
 */
static void
panel_size_alloc (GtkWidget *widget, GdkRectangle *a, gpointer data)
{
    DBG("alloc %d %d\n", a->width, a->height);
}


/*
 * make_round_corners -- apply a bitmap shape mask to round panel corners.
 *
 * Creates a 1-bit GdkBitmap of the same size as the panel, draws filled
 * rectangles and arcs to produce a rounded-rectangle shape, then calls
 * gtk_widget_shape_combine_mask() to clip the panel window to that shape.
 *
 * Parameters:
 *   p - panel instance (uses p->aw, p->ah, p->round_corners_radius).
 *
 * Memory: b (GdkBitmap) and gc (GdkGC) are created and freed locally.
 *
 * Note: If the radius is less than 4 pixels, rounding is skipped (too small
 *       to be meaningful).  Radius is clamped to MIN(w, h) / 2.
 */
static void
make_round_corners(panel *p)
{
    GdkBitmap *b;
    GdkGC* gc;
    GdkColor black = { 0, 0, 0, 0};         /* mask = 0 → transparent */
    GdkColor white = { 1, 0xffff, 0xffff, 0xffff}; /* mask = 1 → opaque */
    int w, h, r, br;

    ENTER;
    w = p->aw;
    h = p->ah;
    r = p->round_corners_radius;
    if (2*r > MIN(w, h)) {
        r = MIN(w, h) / 2;
        DBG("chaning radius to %d\n", r);
    }
    if (r < 4) {
        DBG("radius too small\n");
        RET();
    }
    b = gdk_pixmap_new(NULL, w, h, 1);       /* 1-bit offscreen bitmap */
    gc = gdk_gc_new(GDK_DRAWABLE(b));
    /* fill entire bitmap with black (transparent) */
    gdk_gc_set_foreground(gc, &black);
    gdk_draw_rectangle(GDK_DRAWABLE(b), gc, TRUE, 0, 0, w, h);
    /* draw panel body in white (opaque) */
    gdk_gc_set_foreground(gc, &white);
    gdk_draw_rectangle(GDK_DRAWABLE(b), gc, TRUE, r, 0, w-2*r, h);
    gdk_draw_rectangle(GDK_DRAWABLE(b), gc, TRUE, 0, r, r, h-2*r);
    gdk_draw_rectangle(GDK_DRAWABLE(b), gc, TRUE, w-r, r, r, h-2*r);

    br = 2 * r;
    /* draw four corner arcs */
    gdk_draw_arc(GDK_DRAWABLE(b), gc, TRUE, 0, 0, br, br, 0*64, 360*64);
    gdk_draw_arc(GDK_DRAWABLE(b), gc, TRUE, 0, h-br-1, br, br, 0*64, 360*64);
    gdk_draw_arc(GDK_DRAWABLE(b), gc, TRUE, w-br, 0, br, br, 0*64, 360*64);
    gdk_draw_arc(GDK_DRAWABLE(b), gc, TRUE, w-br, h-br-1, br, br, 0*64, 360*64);

    gtk_widget_shape_combine_mask(p->topgwin, b, 0, 0);  /* apply shape mask */
    g_object_unref(gc);
    g_object_unref(b);

    RET();
}

/*
 * panel_configure_event -- topgwin "configure-event" handler.
 *
 * Called when the panel window is moved or resized.  Compares the new
 * geometry to the requested geometry (p->aw, p->ah, p->ax, p->ay):
 *   - If the window hasn't reached the right size yet, waits for more events.
 *   - If the window is the right size but wrong position, moves it.
 *   - If the window is in the right place, finalises: refreshes the background,
 *     sets the WM strut, applies round corners, and shows the window.
 *
 * Returns: FALSE (does not consume the event).
 */
static gboolean
panel_configure_event(GtkWidget *widget, GdkEventConfigure *e, panel *p)
{
    ENTER;
    DBG("cur geom: %dx%d+%d+%d\n", e->width, e->height, e->x, e->y);
    DBG("req geom: %dx%d+%d+%d\n", p->aw, p->ah, p->ax, p->ay);
    if (e->width == p->cw && e->height == p->ch && e->x == p->cx && e->y ==
            p->cy) {
        DBG("dup. exiting\n");
        RET(FALSE);   /* duplicate event; ignore */
    }
    /* save current geometry */
    p->cw = e->width;
    p->ch = e->height;
    p->cx = e->x;
    p->cy = e->y;

    /* if panel size is not what we have requested, just wait, it will */
    if (e->width != p->aw || e->height != p->ah) {
        DBG("size_req not yet ready. exiting\n");
        RET(FALSE);
    }

    /* if panel wasn't at requested position, then send another request */
    if (e->x != p->ax || e->y != p->ay) {
        DBG("move %d,%d\n", p->ax, p->ay);
        gtk_window_move(GTK_WINDOW(widget), p->ax, p->ay);
        RET(FALSE);
    }

    /* panel is at right place, lets go on */
    DBG("panel is at right place, lets go on\n");
    if (p->transparent) {
        DBG("remake bg image\n");
        fb_bg_notify_changed_bg(p->bg);   /* refresh pseudo-transparent bg */
    }
    if (p->setstrut) {
        DBG("set_wm_strut\n");
        panel_set_wm_strut(p);
    }
    if (p->round_corners) {
        DBG("make_round_corners\n");
        make_round_corners(p);

    }
    gtk_widget_show(p->topgwin);   /* reveal panel to user */
    if (p->setstrut) {
        DBG("set_wm_strut\n");
        panel_set_wm_strut(p);    /* set again after show, for some WMs */
    }
    RET(FALSE);

}

/****************************************************
 *         autohide                                 *
 ****************************************************/

/*
 * Autohide is behaviour when panel hides itself when mouse is "far enough"
 * and pops up again when mouse comes "close enough".
 * Formally, it's a state machine with 3 states that driven by mouse
 * coordinates and timer:
 * 1. VISIBLE - ensures that panel is visible. When/if mouse goes "far enough"
 *      switches to WAITING state
 * 2. WAITING - starts timer. If mouse comes "close enough", stops timer and
 *      switches to VISIBLE.  If timer expires, switches to HIDDEN
 * 3. HIDDEN - hides panel. When mouse comes "close enough" switches to VISIBLE
 *
 * Note 1
 * Mouse coordinates are queried every PERIOD milisec
 *
 * Note 2
 * If mouse is less then GAP pixels to panel it's considered to be close,
 * otherwise it's far
 */

#define GAP 2      /* pixel distance threshold: inside = "close", outside = "far" */
#define PERIOD 300 /* mouse-watch timer interval in milliseconds */

static gboolean ah_state_visible(panel *p);
static gboolean ah_state_waiting(panel *p);
static gboolean ah_state_hidden(panel *p);

/*
 * panel_mapped -- topgwin "map-event" handler.
 *
 * When the panel window becomes visible (mapped), restart the autohide
 * timers so the state machine is in sync with the new window state.
 */
static gboolean
panel_mapped(GtkWidget *widget, GdkEvent *event, panel *p)
{
    ENTER;
    if (p->autohide) {
        ah_stop(p);   /* stop any existing timers */
        ah_start(p);  /* restart fresh from VISIBLE state */
    }
    RET(FALSE);
}

/*
 * mouse_watch -- periodic GLib timeout callback for autohide.
 *
 * Called every PERIOD ms.  Queries the current mouse position and computes
 * p->ah_far (TRUE if the mouse is outside the panel's "close" area).
 * When hidden, the "close" area is reduced to just GAP pixels along the
 * visible panel edge, so the panel doesn't pop up accidentally.
 *
 * After updating ah_far, calls the current state handler p->ah_state(p).
 * Returns: TRUE to keep the timeout repeating.
 */
static gboolean
mouse_watch(panel *p)
{
    gint x, y;

    ENTER;
    gdk_display_get_pointer(gdk_display_get_default(), NULL, &x, &y, NULL);

    gint cx, cy, cw, ch;

    cx = p->cx;
    cy = p->cy;
    cw = p->aw;
    ch = p->ah;

    /* reduce area which will raise panel so it does not interfere with apps */
    if (p->ah_state == ah_state_hidden) {
        switch (p->edge) {
        case EDGE_LEFT:
            cw = GAP;
            break;
        case EDGE_RIGHT:
            cx = cx + cw - GAP;
            cw = GAP;
            break;
        case EDGE_TOP:
            ch = GAP;
            break;
        case EDGE_BOTTOM:
            cy = cy + ch - GAP;
            ch = GAP;
            break;
       }
    }
    p->ah_far = ((x < cx) || (x > cx + cw) || (y < cy) || (y > cy + ch));

    p->ah_state(p);   /* dispatch to current state handler */
    RET(TRUE);
}

/*
 * ah_state_visible -- autohide VISIBLE state handler.
 *
 * Ensures the panel window is shown and sticky.  On first entry, shows the
 * window.  If the mouse is far, transitions to WAITING.
 */
static gboolean
ah_state_visible(panel *p)
{
    ENTER;
    if (p->ah_state != ah_state_visible) {
        /* entering VISIBLE state: show window */
        p->ah_state = ah_state_visible;
        gtk_widget_show(p->topgwin);
        gtk_window_stick(GTK_WINDOW(p->topgwin));
    } else if (p->ah_far) {
        /* mouse has moved far away → start waiting to hide */
        ah_state_waiting(p);
    }
    RET(FALSE);
}

/*
 * ah_state_waiting -- autohide WAITING state handler.
 *
 * On first entry, starts a one-shot timer (2 * PERIOD) to transition to HIDDEN.
 * If the mouse returns close before the timer fires, cancels the timer and
 * transitions back to VISIBLE.
 *
 * Note: hpid is a global variable, which assumes a single panel per process.
 */
static gboolean
ah_state_waiting(panel *p)
{
    ENTER;
    if (p->ah_state != ah_state_waiting) {
        /* entering WAITING state: start hide timer */
        p->ah_state = ah_state_waiting;
        hpid = g_timeout_add(2 * PERIOD, (GSourceFunc) ah_state_hidden, p);
    } else if (!p->ah_far) {
        /* mouse came back close → cancel hide timer, go back to visible */
        g_source_remove(hpid);
        hpid = 0;
        ah_state_visible(p);
    }
    RET(FALSE);
}

/*
 * ah_state_hidden -- autohide HIDDEN state handler.
 *
 * On first entry (triggered by the WAITING timer), hides the panel window.
 * When the mouse enters the "close" zone, transitions back to VISIBLE.
 */
static gboolean
ah_state_hidden(panel *p)
{
    ENTER;
    if (p->ah_state != ah_state_hidden) {
        /* entering HIDDEN state: hide panel window */
        p->ah_state = ah_state_hidden;
        gtk_widget_hide(p->topgwin);
    } else if (!p->ah_far) {
        /* mouse is close → show panel again */
        ah_state_visible(p);
    }
    RET(FALSE);
}

/*
 * ah_start -- start the autohide state machine.
 *
 * Starts the mouse-watcher timer and puts the state machine in VISIBLE.
 * mwid (global) holds the timer source ID.
 */
void
ah_start(panel *p)
{
    ENTER;
    mwid = g_timeout_add(PERIOD, (GSourceFunc) mouse_watch, p);
    ah_state_visible(p);
    RET();
}

/*
 * ah_stop -- stop the autohide state machine.
 *
 * Cancels both the mouse-watcher timer (mwid) and the hide-panel timer
 * (hpid), if they are running.
 */
void
ah_stop(panel *p)
{
    ENTER;
    if (mwid) {
        g_source_remove(mwid);
        mwid = 0;
    }
    if (hpid) {
        g_source_remove(hpid);
        hpid = 0;
    }
    RET();
}

/****************************************************
 *         panel creation                           *
 ****************************************************/

/*
 * about -- show GTK About dialog.
 *
 * Shows standard GTK About dialog with project metadata.
 * Connected to the panel's right-click context menu.
 */
void
about()
{
    gchar *authors[] = { "Anatoly Asviyan <aanatoly@users.sf.net>", NULL };

    ENTER;
    gtk_show_about_dialog(NULL,
        "authors", authors,
        "comments", "Lightweight GTK+ desktop panel",
        "license", "GPLv2",
        "program-name", PROJECT_NAME,
        "version", PROJECT_VERSION,
        "website", "http://fbpanel.sf.net",
        "logo-icon-name", "logo",
        "translator-credits", _("translator-credits"),
        NULL);
    RET();
}

/*
 * panel_make_menu -- create the panel's right-click context menu.
 *
 * Returns a new GtkMenu with:
 *   - "Preferences" → configure()
 *   - separator
 *   - "About"       → about()
 *
 * Memory: the returned GtkMenu is stored in p->menu.  Destroyed in panel_stop().
 */
static GtkWidget *
panel_make_menu(panel *p)
{
    GtkWidget *mi, *menu;

    ENTER;
    menu = gtk_menu_new();

    /* panel's preferences */
    mi = gtk_image_menu_item_new_from_stock(GTK_STOCK_PREFERENCES, NULL);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect_swapped(G_OBJECT(mi), "activate",
        (GCallback)configure, p->xc);
    gtk_widget_show (mi);

    /* separator */
    mi = gtk_separator_menu_item_new();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    gtk_widget_show (mi);

    /* about */
    mi = gtk_image_menu_item_new_from_stock(GTK_STOCK_ABOUT, NULL);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate",
        (GCallback)about, p);
    gtk_widget_show (mi);

    RET(menu);
}

/*
 * panel_button_press_event -- topgwin "button-press-event" handler.
 *
 * Shows the panel's context menu on Ctrl+Right-click.
 * Returns TRUE (event consumed) only if the menu was shown.
 */
gboolean
panel_button_press_event(GtkWidget *widget, GdkEventButton *event, panel *p)
{
    ENTER;
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
          && event->state & GDK_CONTROL_MASK) {
        DBG("ctrl-btn3\n");
        gtk_menu_popup (GTK_MENU (p->menu), NULL, NULL, NULL,
            NULL, event->button, event->time);
        RET(TRUE);
    }
    RET(FALSE);
}

/*
 * panel_scroll_event -- topgwin "scroll-event" handler.
 *
 * Scroll wheel on the panel switches the current virtual desktop:
 *   - Scroll up / left → previous desktop (wrapping).
 *   - Scroll down / right → next desktop (wrapping).
 *
 * Sends _NET_CURRENT_DESKTOP to the root window via Xclimsg().
 */
static gboolean
panel_scroll_event(GtkWidget *widget, GdkEventScroll *event, panel *p)
{
    int i;

    ENTER;
    DBG("scroll direction = %d\n", event->direction);
    i = p->curdesk;
    if (event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_LEFT) {
        i--;
        if (i < 0)
            i = p->desknum - 1;   /* wrap to last desktop */
    } else {
        i++;
        if (i >= p->desknum)
            i = 0;                /* wrap to first desktop */
    }
    Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, i, 0, 0, 0, 0);
    RET(TRUE);
}


/*
 * panel_start_gui -- build the GTK widget hierarchy and configure the window.
 *
 * Called from panel_parse_global() once config is parsed.
 *
 * Widget tree built here:
 *   GtkWindow (topgwin)
 *     └─ GtkBgbox (bbox)           — full-window background
 *          └─ GtkHBox/VBox (lbox)  — outer layout (no spacing)
 *               └─ GtkHBox/VBox (box) — plugin container (p->spacing gap)
 *
 * Also:
 *   - Registers signal handlers on topgwin.
 *   - Realizes the window and gets the X11 window ID (topxwin).
 *   - Installs the root-window PropertyNotify filter.
 *   - Creates the right-click context menu.
 *   - Sets the WM strut if configured.
 *
 * Note: The window is shown then immediately hidden (gtk_widget_show_all +
 *   gtk_widget_hide).  It becomes visible again from panel_configure_event()
 *   once the window manager places it correctly, or from panel_show_anyway()
 *   (200ms timeout) as a fallback.
 *
 * BUG: p->bg (FbBg) is acquired here via fb_bg_get_for_display() but is
 *   never released with g_object_unref() in panel_stop().  The FbBg ref leaks.
 */
static void
panel_start_gui(panel *p)
{
    ENTER;

    /* create the top-level panel window */
    p->topgwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_set_border_width(GTK_CONTAINER(p->topgwin), 0);
    g_signal_connect(G_OBJECT(p->topgwin), "destroy-event",
        (GCallback) panel_destroy_event, p);
    g_signal_connect(G_OBJECT(p->topgwin), "size-request",
        (GCallback) panel_size_req, p);
    g_signal_connect(G_OBJECT(p->topgwin), "size-allocate",
        (GCallback) panel_size_alloc, p);
    g_signal_connect(G_OBJECT(p->topgwin), "map-event",
        (GCallback) panel_mapped, p);
    g_signal_connect(G_OBJECT(p->topgwin), "configure-event",
        (GCallback) panel_configure_event, p);
    g_signal_connect(G_OBJECT(p->topgwin), "button-press-event",
        (GCallback) panel_button_press_event, p);
    g_signal_connect(G_OBJECT(p->topgwin), "scroll-event",
        (GCallback) panel_scroll_event, p);

    gtk_window_set_resizable(GTK_WINDOW(p->topgwin), FALSE);
    gtk_window_set_wmclass(GTK_WINDOW(p->topgwin), "panel", "fbpanel");
    gtk_window_set_title(GTK_WINDOW(p->topgwin), "panel");
    gtk_window_set_position(GTK_WINDOW(p->topgwin), GTK_WIN_POS_NONE);
    gtk_window_set_decorated(GTK_WINDOW(p->topgwin), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(p->topgwin), FALSE);
    if (p->setdocktype)
        /* request DOCK window type so WM excludes it from strut calculations */
        gtk_window_set_type_hint(GTK_WINDOW(p->topgwin),
            GDK_WINDOW_TYPE_HINT_DOCK);

    if (p->layer == LAYER_ABOVE)
        gtk_window_set_keep_above(GTK_WINDOW(p->topgwin), TRUE);
    else if (p->layer == LAYER_BELOW)
        gtk_window_set_keep_below(GTK_WINDOW(p->topgwin), TRUE);
    gtk_window_stick(GTK_WINDOW(p->topgwin));   /* visible on all desktops */

    gtk_widget_realize(p->topgwin);
    p->topxwin = GDK_WINDOW_XWINDOW(p->topgwin->window);  /* get X11 window ID */
    DBG("topxwin = %lx\n", p->topxwin);
    /* ensure configure event by moving the window off-screen briefly */
    XMoveWindow(GDK_DISPLAY(), p->topxwin, 20, 20);
    XSync(GDK_DISPLAY(), False);

    gtk_widget_set_app_paintable(p->topgwin, TRUE);
    calculate_position(p);
    gtk_window_move(GTK_WINDOW(p->topgwin), p->ax, p->ay);
    gtk_window_resize(GTK_WINDOW(p->topgwin), p->aw, p->ah);
    DBG("move-resize x %d y %d w %d h %d\n", p->ax, p->ay, p->aw, p->ah);

    /* create background box covering the entire window */
    p->bbox = gtk_bgbox_new();
    gtk_container_add(GTK_CONTAINER(p->topgwin), p->bbox);
    gtk_container_set_border_width(GTK_CONTAINER(p->bbox), 0);
    if (p->transparent) {
        /* BUG: p->bg is never g_object_unref'd in panel_stop() — leaks FbBg ref */
        p->bg = fb_bg_get_for_display();
        gtk_bgbox_set_background(p->bbox, BG_ROOT, p->tintcolor, p->alpha);
    }

    /* outer layout: no spacing, fills the background box */
    p->lbox = p->my_box_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(p->lbox), 0);
    gtk_container_add(GTK_CONTAINER(p->bbox), p->lbox);

    /* inner plugin container: configured inter-plugin spacing */
    p->box = p->my_box_new(FALSE, p->spacing);
    gtk_container_set_border_width(GTK_CONTAINER(p->box), 0);
    gtk_box_pack_start(GTK_BOX(p->lbox), p->box, TRUE, TRUE,
        (p->round_corners) ? p->round_corners_radius : 0);
    if (p->round_corners) {
        DBG("make_round_corners\n");
        make_round_corners(p);
    }
    /* show all widgets, then immediately hide the top-level window */
    gtk_widget_show_all(p->topgwin);
    gtk_widget_hide(p->topgwin);  /* will be shown by configure-event or timer */

    p->menu = panel_make_menu(p);

    if (p->setstrut)
        panel_set_wm_strut(p);

    /* listen for PropertyNotify events on the root window */
    XSelectInput(GDK_DISPLAY(), GDK_ROOT_WINDOW(), PropertyChangeMask);
    gdk_window_add_filter(gdk_get_default_root_window(),
          (GdkFilterFunc)panel_event_filter, p);
    gdk_flush();
    RET();
}

/*
 * panel_parse_global -- parse the "global" config block and initialise panel.
 *
 * Sets default values for all panel fields, then reads from the xconf tree.
 * After reading config, performs sanity clamping on width/height/alpha, sets
 * orientation-dependent function pointers (my_box_new, my_separator_new),
 * fetches initial desktop state from X11, and calls panel_start_gui().
 *
 * Parameters:
 *   xc - the "global" xconf sub-tree.
 *
 * Returns: 1 (always; panel_start_gui does not fail).
 *
 * BUG: Line ~716 unconditionally sets `p->heighttype = HEIGHT_PIXEL` AFTER
 *   reading it from config.  This overwrites whatever heighttype was read,
 *   so HEIGHT_REQUEST and HEIGHT_PIXEL are the only effective values even
 *   though HEIGHT_PERCENT is defined.
 */
static int
panel_parse_global(xconf *xc)
{
    ENTER;
    /* Set default values */
    p->allign = ALLIGN_CENTER;
    p->edge = EDGE_BOTTOM;
    p->widthtype = WIDTH_PERCENT;
    p->width = 100;
    p->heighttype = HEIGHT_PIXEL;
    p->height = PANEL_HEIGHT_DEFAULT;
    p->max_elem_height = PANEL_HEIGHT_MAX;
    p->setdocktype = 1;
    p->setstrut = 1;
    p->round_corners = 1;
    p->round_corners_radius = 7;
    p->autohide = 0;
    p->height_when_hidden = 2;
    p->transparent = 0;
    p->alpha = 127;
    p->tintcolor_name = "white";
    p->spacing = 0;
    p->setlayer = FALSE;
    p->layer = LAYER_ABOVE;

    /* Read config */
    /* geometry */
    XCG(xc, "edge", &p->edge, enum, edge_enum);
    XCG(xc, "allign", &p->allign, enum, allign_enum);
    XCG(xc, "widthtype", &p->widthtype, enum, widthtype_enum);
    XCG(xc, "heighttype", &p->heighttype, enum, heighttype_enum);
    XCG(xc, "width", &p->width, int);
    XCG(xc, "height", &p->height, int);
    XCG(xc, "xmargin", &p->xmargin, int);
    XCG(xc, "ymargin", &p->ymargin, int);

    /* properties */
    XCG(xc, "setdocktype", &p->setdocktype, enum, bool_enum);
    XCG(xc, "setpartialstrut", &p->setstrut, enum, bool_enum);
    XCG(xc, "autohide", &p->autohide, enum, bool_enum);
    XCG(xc, "heightwhenhidden", &p->height_when_hidden, int);
    XCG(xc, "setlayer", &p->setlayer, enum, bool_enum);
    XCG(xc, "layer", &p->layer, enum, layer_enum);

    /* effects */
    XCG(xc, "roundcorners", &p->round_corners, enum, bool_enum);
    XCG(xc, "roundcornersradius", &p->round_corners_radius, int);
    XCG(xc, "transparent", &p->transparent, enum, bool_enum);
    XCG(xc, "alpha", &p->alpha, int);
    XCG(xc, "tintcolor", &p->tintcolor_name, str);
    XCG(xc, "maxelemheight", &p->max_elem_height, int);

    /* Sanity checks */
    if (!gdk_color_parse(p->tintcolor_name, &p->gtintcolor))
        gdk_color_parse("white", &p->gtintcolor);
    p->tintcolor = gcolor2rgb24(&p->gtintcolor);
    DBG("tintcolor=%x\n", p->tintcolor);
    if (p->alpha > 255)
        p->alpha = 255;
    p->orientation = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM)
        ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;
    if (p->orientation == GTK_ORIENTATION_HORIZONTAL) {
        p->my_box_new = gtk_hbox_new;
        p->my_separator_new = gtk_vseparator_new;
    } else {
        p->my_box_new = gtk_vbox_new;
        p->my_separator_new = gtk_hseparator_new;
    }
    if (p->width < 0)
        p->width = 100;
    if (p->widthtype == WIDTH_PERCENT && p->width > 100)
        p->width = 100;
    /* BUG: this line unconditionally sets heighttype to HEIGHT_PIXEL, overriding
     * the value just read from config above.  Any configured heighttype other
     * than HEIGHT_PIXEL is silently discarded. */
    p->heighttype = HEIGHT_PIXEL;
    if (p->heighttype == HEIGHT_PIXEL) {
        if (p->height < PANEL_HEIGHT_MIN)
            p->height = PANEL_HEIGHT_MIN;
        else if (p->height > PANEL_HEIGHT_MAX)
            p->height = PANEL_HEIGHT_MAX;
    }
    if (p->max_elem_height > p->height ||
            p->max_elem_height < PANEL_HEIGHT_MIN)
        p->max_elem_height = p->height;
    p->curdesk = get_net_current_desktop();
    p->desknum = get_net_number_of_desktops();
    panel_start_gui(p);
    RET(1);
}

/*
 * panel_parse_plugin -- parse one "plugin" config block and load the plugin.
 *
 * Reads the "type" key, loads the plugin .so via plugin_load(), sets panel
 * and config pointers, reads expand/padding/border options, then starts the
 * plugin.
 *
 * Parameters:
 *   xc - a "plugin" xconf sub-tree.
 *
 * BUG: calls exit(1) if plugin_start() fails, which skips panel_stop()
 *   cleanup.  A graceful error (skip the plugin or abort cleanly) would be
 *   safer.
 */
static void
panel_parse_plugin(xconf *xc)
{
    plugin_instance *plug = NULL;
    gchar *type = NULL;

    ENTER;
    xconf_get_str(xconf_find(xc, "type", 0), &type);
    if (!type || !(plug = plugin_load(type))) {
        ERR( "fbpanel: can't load %s plugin\n", type);
        return;
    }
    plug->panel = p;
    XCG(xc, "expand", &plug->expand, enum, bool_enum);
    XCG(xc, "padding", &plug->padding, int);
    XCG(xc, "border", &plug->border, int);
    plug->xc = xconf_find(xc, "config", 0);  /* non-owning pointer into xc sub-tree */

    if (!plugin_start(plug)) {
        ERR( "fbpanel: can't start plugin %s\n", type);
        exit(1);   /* BUG: exits without cleanup */
    }
    p->plugins = g_list_append(p->plugins, plug);
}

/*
 * panel_show_anyway -- fallback timeout callback to show panel.
 *
 * Scheduled with a 200ms delay in panel_start() as a fallback in case
 * panel_configure_event() never shows the window (e.g., no WM present).
 *
 * Returns: FALSE to remove itself from the GLib main loop.
 */
static gboolean
panel_show_anyway(gpointer data)
{
    ENTER;
    gtk_widget_show_all(p->topgwin);
    return FALSE;
}


/*
 * panel_start -- top-level panel initialisation sequence.
 *
 * Creates the FbEv singleton, parses the "global" config block (which builds
 * the GUI), and then loads each "plugin" block from the config tree.
 * Schedules a 200ms fallback show timeout.
 *
 * Parameters:
 *   xc - the root xconf tree (profile config).
 */
static void
panel_start(xconf *xc)
{
    int i;
    xconf *pxc;

    ENTER;
    fbev = fb_ev_new();   /* create FbEv event-bus singleton */

    panel_parse_global(xconf_find(xc, "global", 0));
    for (i = 0; (pxc = xconf_find(xc, "plugin", i)); i++)
        panel_parse_plugin(pxc);
    /* Fallback: show panel 200ms after startup regardless of configure-event */
    g_timeout_add(200, panel_show_anyway, NULL);
    RET();
}

/*
 * delete_plugin -- g_list_foreach callback to stop and unload a plugin.
 *
 * Called from panel_stop() to destroy each loaded plugin in order.
 */
static void
delete_plugin(gpointer data, gpointer udata)
{
    ENTER;
    plugin_stop((plugin_instance *)data);   /* call plugin's destructor */
    plugin_put((plugin_instance *)data);    /* unload .so, free memory */
    RET();
}

/*
 * panel_stop -- tear down the panel and release all resources.
 *
 * Order of operations:
 *   1. Stop autohide timers.
 *   2. Destroy all loaded plugins (stop + put).
 *   3. Remove root-window X11 event filter.
 *   4. Destroy topgwin (which cascades to all child widgets).
 *   5. Destroy context menu.
 *   6. Unref FbEv singleton.
 *   7. Flush X11 event queues.
 *
 * Note: p->bg (FbBg) is NOT unreffed here — this is a memory leak.
 *   Fix: if (p->transparent && p->bg) g_object_unref(p->bg);
 */
static void
panel_stop(panel *p)
{
    ENTER;

    if (p->autohide)
        ah_stop(p);                      /* stop autohide timers */
    g_list_foreach(p->plugins, delete_plugin, NULL);
    g_list_free(p->plugins);
    p->plugins = NULL;

    XSelectInput(GDK_DISPLAY(), GDK_ROOT_WINDOW(), NoEventMask);
    gdk_window_remove_filter(gdk_get_default_root_window(),
          (GdkFilterFunc)panel_event_filter, p);
    gtk_widget_destroy(p->topgwin);   /* destroys all child widgets recursively */
    gtk_widget_destroy(p->menu);      /* menu is not a child of topgwin */
    g_object_unref(fbev);             /* release FbEv singleton */
    /* BUG: p->bg (FbBg) not g_object_unref'd here if p->transparent was set */
    gdk_flush();
    XFlush(GDK_DISPLAY());
    XSync(GDK_DISPLAY(), True);
    RET();
}

/*
 * usage -- print command-line help and exit.
 */
void
usage()
{
    ENTER;
    printf("fbpanel %s - lightweight GTK2+ panel for UNIX desktops\n", version);
    printf("Command line options:\n");
    printf(" --help      -- print this help and exit\n");
    printf(" --version   -- print version and exit\n");
    printf(" --log <number> -- set log level 0-5. 0 - none 5 - chatty\n");
    printf(" --configure -- launch configuration utility\n");
    printf(" --profile name -- use specified profile\n");
    printf("\n");
    printf(" -h  -- same as --help\n");
    printf(" -p  -- same as --profile\n");
    printf(" -v  -- same as --version\n");
    printf(" -C  -- same as --configure\n");
    printf("\nVisit http://fbpanel.sourceforge.net/ for detailed documentation,\n\n");
}

/*
 * handle_error -- X11 error handler.
 *
 * Installed via XSetErrorHandler() in main().
 * Logs X errors via DBG() (not ERR(), so only visible at high log levels).
 * Does not abort — fbpanel continues after X errors.
 */
void
handle_error(Display * d, XErrorEvent * ev)
{
    char buf[256];

    ENTER;
    XGetErrorText(GDK_DISPLAY(), ev->error_code, buf, 256);
    DBG("fbpanel : X error: %s\n", buf);

    RET();
}

/*
 * sig_usr1 -- SIGUSR1 handler: reload config (restart panel).
 *
 * Calls gtk_main_quit() without setting force_quit, so the restart loop
 * will restart the panel with a fresh config read.
 */
static void
sig_usr1(int signum)
{
    if (signum != SIGUSR1)
        return;
    gtk_main_quit();
}

/*
 * sig_usr2 -- SIGUSR2 handler: quit fbpanel cleanly.
 *
 * Calls gtk_main_quit() and sets force_quit=1 so the restart loop exits.
 */
static void
sig_usr2(int signum)
{
    if (signum != SIGUSR2)
        return;
    gtk_main_quit();
    force_quit = 1;
}

/*
 * do_argv -- parse command-line arguments.
 *
 * Processes --help, --version, --log, --configure, --profile, --xineramaHead
 * and their short-form equivalents.
 * Exits the process directly for --help and --version.
 */
static void
do_argv(int argc, char *argv[])
{
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            exit(0);
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("fbpanel %s\n", version);
            exit(0);
        } else if (!strcmp(argv[i], "--log")) {
            i++;
            if (i == argc) {
                ERR( "fbpanel: missing log level\n");
                usage();
                exit(1);
            } else {
                log_level = atoi(argv[i]);
            }
        } else if (!strcmp(argv[i], "--configure") || !strcmp(argv[i], "-C")) {
            config = 1;
        } else if (!strcmp(argv[i], "--profile") || !strcmp(argv[i], "-p")) {
            i++;
            if (i == argc) {
                ERR( "fbpanel: missing profile name\n");
                usage();
                exit(1);
            } else {
                profile = g_strdup(argv[i]);
            }
        } else if (!strcmp(argv[i], "--xineramaHead") ||
                   !strcmp(argv[i], "-x")) {
          i++;
          if(i == argc) {
            ERR("fbpanel: xinerama head not specified\n");
            usage();
            exit(1);
          }
          xineramaHead = atoi(argv[i]);
        } else {
            printf("fbpanel: unknown option - %s\n", argv[i]);
            usage();
            exit(1);
        }
    }
}

/* panel_get_profile -- return the active profile name (e.g. "default"). */
gchar *panel_get_profile()
{
    return profile;
}

/* panel_get_profile_file -- return the full path to the profile config file. */
gchar *panel_get_profile_file()
{
    return profile_file;
}

/*
 * ensure_profile -- create the profile config file if it does not exist.
 *
 * If the profile file is missing, runs LIBEXECDIR/fbpanel/make_profile <name>
 * to generate it from a default template.  Exits if the profile still cannot
 * be found after the generation attempt.
 */
static void
ensure_profile()
{
    gchar *cmd;

    if (g_file_test(profile_file,
            G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
    {
        return;   /* profile already exists */
    }
    cmd = g_strdup_printf("%s %s", LIBEXECDIR "/fbpanel/make_profile",
        profile);
    g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
    g_free(cmd);
    if (g_file_test(profile_file,
            G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
    {
        return;   /* profile was created */
    }
    ERR("Can't open profile %s - %s\n", profile, profile_file);
    exit(1);
}

/*
 * main -- fbpanel entry point.
 *
 * Initialises locale, GTK, X11, and GLib, then enters the restart loop:
 *
 *   do {
 *       allocate panel struct
 *       parse config from profile_file
 *       panel_start() → build GUI, load plugins, run gtk_main()
 *       panel_stop()  → destroy plugins and widgets
 *       free panel struct
 *   } while (force_quit == 0);
 *
 * The restart loop allows SIGUSR1 to reload the config without restarting
 * the process.  force_quit is set to 1 by SIGUSR2 or the window manager
 * destroying the panel.
 */
int
main(int argc, char *argv[])
{
    setlocale(LC_CTYPE, "");
    bindtextdomain(PROJECT_NAME, LOCALEDIR);
    textdomain(PROJECT_NAME);

    gtk_set_locale();
    gtk_init(&argc, &argv);
    XSetLocaleModifiers("");
    XSetErrorHandler((XErrorHandler) handle_error);
    fb_init();           /* intern X11 atoms; create fbev */
    do_argv(argc, argv);
    profile_file = g_build_filename(g_get_user_config_dir(),
        "fbpanel", profile, NULL);
    ensure_profile();
    gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), IMGPREFIX);
    signal(SIGUSR1, sig_usr1);   /* reload config */
    signal(SIGUSR2, sig_usr2);   /* quit */

    /* restart loop: each iteration = one panel lifetime */
    do {
        the_panel = p = g_new0(panel, 1);
        p->xineramaHead = xineramaHead;
        p->xc = xconf_new_from_file(profile_file, profile);
        if (!p->xc)
            exit(1);

        panel_start(p->xc);
        if (config)
            configure(p->xc);   /* open preferences dialog if -C given */
        gtk_main();             /* run GTK event loop */
        panel_stop(p);
        xconf_del(p->xc, FALSE);
        g_free(p);
        DBG("force_quit=%d\n", force_quit);
    } while (force_quit == 0);
    g_free(profile_file);
    fb_free();   /* free X11 atoms and fbev */
    exit(0);
}

