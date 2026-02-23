/* pager.c -- fbpanel desktop pager plugin.
 *
 * Copyright (C) 2002-2003 Anatoly Asviyan <aanatoly@users.sf.net>
 *                         Joe MacDonald   <joe@deserted.net>
 *
 * Displays thumbnail miniatures of all virtual desktops, with small
 * coloured rectangles representing open windows.  Click a thumbnail to
 * switch to that desktop.
 *
 * Data structures:
 *   task  — tracks one managed window: geometry, desktop, nws/nwwt, icon.
 *   desk  — one desktop thumbnail: GtkDrawingArea + backing GdkPixmap.
 *   pager_priv — top-level plugin state: array of desk*, GHashTable of tasks.
 *
 * Event sources:
 *   FbEv signals: current_desktop, active_window, number_of_desktops,
 *                 client_list_stacking.
 *   GDK root-window filter: pager_event_filter() handles PropertyNotify
 *     (window state/desktop changes) and ConfigureNotify (window moves/resizes)
 *     on client windows.
 *
 * Rendering:
 *   Each desk has two GdkPixmap backing buffers:
 *     d->pix  — composited task rectangles drawn into this buffer.
 *     d->gpix — scaled wallpaper (from FbBg) drawn into this buffer.
 *   desk_expose_event() blits d->pix → widget window on each expose.
 *   d->dirty = 1 triggers a full redraw on the next expose.
 *
 * Wallpaper:
 *   If pg->wallpaper is enabled (default), FbBg is used to fetch the root
 *   pixmap, scale it to thumbnail size, and composite it into d->gpix.
 *   Non-zero desk indices share the background from desk[0] when possible.
 *
 * Fixed bugs:
 *   Fixed (BUG-006): desk_configure_event() now correctly sets
 *     scalew = widget_w / screen_w and scaleh = widget_h / screen_h.
 *   Fixed (BUG-007): pg->gen_pixbuf is now g_object_unref'd in
 *     pager_destructor.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>


#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf-xlib/gdk-pixbuf-xlib.h>
#include <gdk/gdk.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "data/images/default.xpm"
#include "gtkbgbox.h"

//#define DEBUGPRN
#include "dbg.h"



/*
 * task -- one managed X11 window tracked by the pager.
 *
 * win      - X11 Window ID.
 * x,y,w,h  - last-known window geometry in root-window coordinates (pixels).
 * refcount - reference count for stale-window detection in do_net_client_list.
 * stacking - stacking order index from _NET_CLIENT_LIST_STACKING.
 * desktop  - virtual desktop number (0-based); 0xFFFFFFFF = all desktops.
 * name     - window title (not displayed; for debugging).
 * iname    - iconified window title (not displayed; for debugging).
 * nws      - _NET_WM_STATE flags (hidden, shaded, skip_pager, …).
 * nwwt     - _NET_WM_WINDOW_TYPE flags (desktop, dock, splash, …).
 * pixbuf   - application icon (netwm or WM hint; 16×16 px).
 * using_netwm_icon - 1 if pixbuf came from _NET_WM_ICON.
 */
typedef struct _task {
    Window win;
    int x, y;
    guint w, h;
    gint refcount;
    guint stacking;
    guint desktop;
    char *name, *iname;
    net_wm_state nws;
    net_wm_window_type nwwt;
    GdkPixbuf *pixbuf;
    unsigned int using_netwm_icon:1;
} task;

typedef struct _desk   desk;
typedef struct _pager_priv  pager_priv;

#define MAX_DESK_NUM   20   /* maximum number of supported virtual desktops */

/*
 * desk -- one virtual desktop thumbnail.
 *
 * da     - GtkDrawingArea widget for this desktop's thumbnail.
 * xpix   - X11 Pixmap ID of the root background (None if not available).
 * gpix   - GdkPixmap of scaled wallpaper (only used when pg->wallpaper).
 * pix    - GdkPixmap backing buffer (task rectangles composited here).
 * no     - desktop index (0-based).
 * dirty  - 1 if pix needs to be redrawn before the next blit.
 * first  - 1 before the first configure event (unused after init).
 * scalew - horizontal scale factor: thumbnail_w / screen_w.
 * scaleh - vertical scale factor:   thumbnail_h / screen_h.
 * pg     - back-pointer to pager_priv.
 */
struct _desk {
    GtkWidget *da;
    Pixmap xpix;
    GdkPixmap *gpix;
    GdkPixmap *pix;
    guint no, dirty, first;
    gfloat scalew, scaleh;
    pager_priv *pg;
};

/*
 * pager_priv -- private state for one pager plugin instance.
 *
 * plugin     - embedded plugin_instance (MUST be first).
 * box        - orientation-aligned box holding all desk->da widgets.
 * desks[]    - array of up to MAX_DESK_NUM desk pointers.
 * desknum    - current number of virtual desktops.
 * curdesk    - current desktop index.
 * wallpaper  - non-zero if root pixmap should be shown in thumbnails.
 * ratio      - screen_width / screen_height (used to size thumbnails).
 * wins       - XFree-able window list from _NET_CLIENT_LIST_STACKING.
 * winnum     - number of entries in wins.
 * dirty      - unused (kept for potential future use).
 * htable     - GHashTable mapping Window → task*.
 * focusedtask - task corresponding to the currently active window.
 * fbbg       - FbBg reference for wallpaper rendering (NULL if !wallpaper).
 * dah,daw   - desk area height and width in pixels.
 * gen_pixbuf - default application icon (loaded from default.xpm).
 *              Unreferenced in pager_destructor (BUG-007 fix).
 */
struct _pager_priv {
    plugin_instance plugin;
    GtkWidget *box;
    desk *desks[MAX_DESK_NUM];
    guint desknum;
    guint curdesk;
    gint wallpaper;
    gfloat ratio;
    Window *wins;
    int winnum, dirty;
    GHashTable* htable;
    task *focusedtask;
    FbBg *fbbg;
    gint dah, daw;
    GdkPixbuf *gen_pixbuf;
};



/* A task is visible if it is not hidden and not explicitly skip_pager */
#define TASK_VISIBLE(tk)                            \
 (!( (tk)->nws.hidden || (tk)->nws.skip_pager ))


static void pager_rebuild_all(FbEv *ev, pager_priv *pg);
static void desk_draw_bg(pager_priv *pg, desk *d1);

static void pager_destructor(plugin_instance *p);

static inline void desk_set_dirty_by_win(pager_priv *p, task *t);
static inline void desk_set_dirty(desk *d);
static inline void desk_set_dirty_all(pager_priv *pg);

/*
static void desk_clear_pixmap(desk *d);
static gboolean task_remove_stale(Window *win, task *t, pager_priv *p);
static gboolean task_remove_all(Window *win, task *t, pager_priv *p);
*/

#ifdef EXTRA_DEBUG
static pager_priv *cp;

/* Debug signal handler: prints all tracked window IDs on SIGUSR2 */
static void
sig_usr(int signum)
{
    int j;
    task *t;

    if (signum != SIGUSR2)
        return;
    ERR("dekstop num=%d cur_desktop=%d\n", cp->desknum, cp->curdesk);
    for (j = 0; j < cp->winnum; j++) {
        if (!(t = g_hash_table_lookup(cp->htable, &cp->wins[j])))
            continue;
        ERR("win=%x desktop=%u\n", (guint) t->win, t->desktop);
    }

}
#endif


/*****************************************************************
 * Task Management Routines                                      *
 *****************************************************************/


/*
 * task_remove_stale -- GHashTable foreach-remove callback.
 *
 * Decrements t->refcount; removes the task (and marks its desk dirty)
 * if refcount reaches 0.  Called after each _NET_CLIENT_LIST_STACKING
 * refresh to remove windows that disappeared from the stacking list.
 *
 * Returns: TRUE to remove from the hash table, FALSE to keep.
 */
static gboolean
task_remove_stale(Window *win, task *t, pager_priv *p)
{
    if (t->refcount-- == 0) {
        desk_set_dirty_by_win(p, t);
        if (p->focusedtask == t)
            p->focusedtask = NULL;
        DBG("del %lx\n", t->win);
        g_free(t);
        return TRUE;
    }
    return FALSE;
}

/*
 * task_remove_all -- GHashTable foreach-remove callback.
 *
 * Unconditionally removes and frees a task, unreffing its pixbuf if set.
 * Used during desktop count changes and destructor cleanup.
 *
 * Returns: TRUE (always remove).
 */
static gboolean
task_remove_all(Window *win, task *t, pager_priv *p)
{
    if (t->pixbuf != NULL)
        g_object_unref(t->pixbuf);

    g_free(t);
    return TRUE;
}


/*
 * task_get_sizepos -- update t->x,y,w,h from X11 window geometry.
 *
 * Tries XGetWindowAttributes first (gives root-relative coordinates via
 * XTranslateCoordinates), then falls back to XGetGeometry.  On complete
 * failure, sets all fields to 2 (minimal non-zero placeholder).
 */
static void
task_get_sizepos(task *t)
{
    Window root, junkwin;
    int rx, ry;
    guint dummy;
    XWindowAttributes win_attributes;

    ENTER;
    if (!XGetWindowAttributes(GDK_DISPLAY(), t->win, &win_attributes)) {
        if (!XGetGeometry (GDK_DISPLAY(), t->win, &root, &t->x, &t->y, &t->w, &t->h,
                  &dummy, &dummy)) {
            t->x = t->y = t->w = t->h = 2;   /* fallback sentinel */
        }

    } else {
        /* translate window-relative (0,0) to root-window coordinates */
        XTranslateCoordinates (GDK_DISPLAY(), t->win, win_attributes.root,
              -win_attributes.border_width,
              -win_attributes.border_width,
              &rx, &ry, &junkwin);
        t->x = rx;
        t->y = ry;
        t->w = win_attributes.width;
        t->h = win_attributes.height;
        DBG("win=0x%lx WxH=%dx%d\n", t->win,t->w, t->h);
    }
    RET();
}


/*
 * task_update_pix -- draw one task's rectangle and icon into d->pix.
 *
 * Skips invisible tasks and tasks on other desktops.
 * Draws a filled rectangle (background colour) and an outline
 * (foreground colour), using GTK_STATE_SELECTED for the focused task
 * and GTK_STATE_NORMAL for all others.
 *
 * If the rectangle is ≥ 10×10 px, the task's pixbuf (or gen_pixbuf) is
 * centred inside it.  Small icons are scaled down to fit.
 *
 * Parameters:
 *   t - task to draw.
 *   d - desk whose d->pix receives the drawing.
 */
static void
task_update_pix(task *t, desk *d)
{
    int x, y, w, h;
    GtkWidget *widget;

    ENTER;
    g_return_if_fail(d->pix != NULL);
    if (!TASK_VISIBLE(t))
        RET();

    /* skip tasks on other desktops (0xFFFF... = sticky = show on all) */
    if (t->desktop < d->pg->desknum &&
          t->desktop != d->no)
        RET();

    /* scale task screen coordinates to thumbnail coordinates */
    x = (gfloat)t->x * d->scalew;
    y = (gfloat)t->y * d->scaleh;
    w = (gfloat)t->w * d->scalew;
    /* shaded windows are drawn as a thin 3px strip */
    h = (t->nws.shaded) ? 3 : (gfloat)t->h * d->scaleh;
    if (w < 3 || h < 3)
        RET();   /* too small to be worth drawing */
    widget = GTK_WIDGET(d->da);
    /* filled background */
    gdk_draw_rectangle (d->pix,
          (d->pg->focusedtask == t) ?
          widget->style->bg_gc[GTK_STATE_SELECTED] :
          widget->style->bg_gc[GTK_STATE_NORMAL],
          TRUE,
          x+1, y+1, w-1, h-1);
    /* outline border */
    gdk_draw_rectangle (d->pix,
          (d->pg->focusedtask == t) ?
          widget->style->fg_gc[GTK_STATE_SELECTED] :
          widget->style->fg_gc[GTK_STATE_NORMAL],
          FALSE,
          x, y, w-1, h);

    if (w>=10 && h>=10) {
        GdkPixbuf* source_buf = t->pixbuf;
        if (source_buf == NULL)
            source_buf = d->pg->gen_pixbuf;   /* fall back to default icon */

        /* scale icon down if the window thumbnail is smaller than 18px */
        GdkPixbuf* scaled = source_buf;
        int scale = 16;
        int noscale = 1;
        int smallest = ( (w<h) ? w : h );
        if (smallest < 18) {
            noscale = 0;
            scale = smallest - 2;
            if (scale % 2 != 0)
                scale++;

            scaled = gdk_pixbuf_scale_simple(source_buf,
                                    scale, scale,
                                    GDK_INTERP_BILINEAR);
        }

        /* centre icon in the task rectangle */
        int pixx = x+((w/2)-(scale/2))+1;
        int pixy = y+((h/2)-(scale/2))+1;

        gdk_draw_pixbuf(d->pix,
                NULL,
                scaled,
                0, 0,
                pixx, pixy,
                -1, -1,
                GDK_RGB_DITHER_NONE,
                0, 0);

        /* free the scaled copy only if we created it (and it's not the default) */
        if (!noscale && t->pixbuf != NULL)
            g_object_unref(scaled);
    }
    RET();
}


/*****************************************************************
 * Desk Functions                                                *
 *****************************************************************/

/*
 * desk_clear_pixmap -- fill d->pix with the background colour or wallpaper.
 *
 * If wallpaper is enabled and d->gpix is ready, blits the scaled wallpaper
 * pixmap; otherwise fills with GTK_STATE_SELECTED (current desk) or
 * GTK_STATE_NORMAL (other desks) dark colour.
 *
 * Also draws a selection border around the current desktop if wallpaper
 * is enabled.
 */
static void
desk_clear_pixmap(desk *d)
{
    GtkWidget *widget;

    ENTER;
    DBG("d->no=%d\n", d->no);
    if (!d->pix)
        RET();
    widget = GTK_WIDGET(d->da);
    if (d->pg->wallpaper && d->xpix != None) {
        /* blit the scaled wallpaper from gpix */
        gdk_draw_drawable (d->pix,
              widget->style->dark_gc[GTK_STATE_NORMAL],
              d->gpix,
              0, 0, 0, 0,
              widget->allocation.width,
              widget->allocation.height);
    } else {
        /* solid background; current desktop uses selected colour */
        gdk_draw_rectangle (d->pix,
              ((d->no == d->pg->curdesk) ?
                    widget->style->dark_gc[GTK_STATE_SELECTED] :
                    widget->style->dark_gc[GTK_STATE_NORMAL]),
              TRUE,
              0, 0,
              widget->allocation.width,
              widget->allocation.height);
    }
    /* highlight border for current desktop when wallpaper is showing */
    if (d->pg->wallpaper && d->no == d->pg->curdesk)
        gdk_draw_rectangle (d->pix,
              widget->style->light_gc[GTK_STATE_SELECTED],
              FALSE,
              0, 0,
              widget->allocation.width -1,
              widget->allocation.height -1);
    RET();
}


/*
 * desk_draw_bg -- fetch and scale the root pixmap into d->gpix.
 *
 * Non-zero desks copy the wallpaper from desk[0]'s gpix if it has the same
 * dimensions (avoids repeated fetches from FbBg).  Desk 0 always fetches
 * fresh via fb_bg_get_xroot_pix_for_area().
 *
 * The wallpaper is:
 *   1. Fetched as a GdkPixmap for the entire screen.
 *   2. Converted to a GdkPixbuf.
 *   3. Scaled (GDK_INTERP_HYPER) to the thumbnail size.
 *   4. Drawn back into d->gpix.
 *   5. d->xpix is set to the X11 root pixmap ID for change detection.
 *
 * Parameters:
 *   pg - pager_priv instance.
 *   d1 - desk whose wallpaper is being prepared.
 */
static void
desk_draw_bg(pager_priv *pg, desk *d1)
{
    Pixmap xpix;
    GdkPixmap *gpix;
    GdkPixbuf *p1, *p2;
    gint width, height, depth;
    FbBg *bg = pg->fbbg;
    GtkWidget *widget = d1->da;

    ENTER;
    /* non-zero desks: try to share wallpaper from desk[0] */
    if (d1->no) {
        desk *d0 = d1->pg->desks[0];
        if (d0->gpix && d0->xpix != None
              && d0->da->allocation.width == widget->allocation.width
              && d0->da->allocation.height == widget->allocation.height) {
            gdk_draw_drawable(d1->gpix,
                  widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
                  d0->gpix,0, 0, 0, 0,
                  widget->allocation.width,
                  widget->allocation.height);
            d1->xpix = d0->xpix;
            DBG("copy gpix from d0 to d%d\n", d1->no);
            RET();
        }
    }
    xpix = fb_bg_get_xrootpmap(bg);
    d1->xpix = None;
    width = widget->allocation.width;
    height = widget->allocation.height;
    DBG("w %d h %d\n", width, height);
    if (width < 3 || height < 3)
        RET();

    /* fetch full-screen root pixmap and scale it to thumbnail size */
    xpix = fb_bg_get_xrootpmap(bg);
    if (xpix == None)
        RET();
    depth = gdk_drawable_get_depth(widget->window);
    gpix = fb_bg_get_xroot_pix_for_area(bg, 0, 0, gdk_screen_width(), gdk_screen_height(), depth);
    if (!gpix) {
        ERR("fb_bg_get_xroot_pix_for_area failed\n");
        RET();
    }
    p1 = gdk_pixbuf_get_from_drawable(NULL, gpix, NULL, 0, 0, 0, 0,
          gdk_screen_width(), gdk_screen_height());
    if (!p1) {
        ERR("gdk_pixbuf_get_from_drawable failed\n");
        goto err_gpix;
    }
    p2 = gdk_pixbuf_scale_simple(p1, width, height,
          GDK_INTERP_HYPER   /* highest quality */
        );
    if (!p2) {
        ERR("gdk_pixbuf_scale_simple failed\n");
        goto err_p1;
    }
    gdk_draw_pixbuf(d1->gpix, widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
          p2, 0, 0, 0, 0, width, height,  GDK_RGB_DITHER_NONE, 0, 0);

    d1->xpix = xpix;   /* record root pixmap ID for change detection */
    g_object_unref(p2);
 err_p1:
    g_object_unref(p1);
 err_gpix:
    g_object_unref(gpix);
    RET();
}



/*
 * desk_set_dirty -- mark a desk for redraw and queue a widget expose.
 */
static inline void
desk_set_dirty(desk *d)
{
    ENTER;
    d->dirty = 1;
    gtk_widget_queue_draw(d->da);
    RET();
}

/*
 * desk_set_dirty_all -- mark all desks dirty (e.g. after a wallpaper change).
 */
static inline void
desk_set_dirty_all(pager_priv *pg)
{
    int i;
    ENTER;
    for (i = 0; i < pg->desknum; i++)
        desk_set_dirty(pg->desks[i]);
    RET();
}

/*
 * desk_set_dirty_by_win -- mark the desk(s) affected by a task change.
 *
 * Skips dock/desktop/splash windows and skip_pager windows.
 * Marks the task's specific desktop if t->desktop < desknum; otherwise
 * marks all desktops (sticky window = could be on any desktop).
 */
static inline void
desk_set_dirty_by_win(pager_priv *p, task *t)
{
    ENTER;
    if (t->nws.skip_pager || t->nwwt.desktop)
        RET();
    if (t->desktop < p->desknum)
        desk_set_dirty(p->desks[t->desktop]);
    else
        desk_set_dirty_all(p);
    RET();
}

/*
 * desk_expose_event -- "expose_event" handler: blit backing pixmap to screen.
 *
 * If d->dirty, recomposites the thumbnail (background + task rectangles)
 * into d->pix before blitting.  Only the event->area region is blitted.
 *
 * Parameters:
 *   widget - the GtkDrawingArea for this desktop.
 *   event  - expose event (clipping rect).
 *   d      - desk being exposed.
 *
 * Returns: FALSE (allow further handlers).
 */
static gint
desk_expose_event (GtkWidget *widget, GdkEventExpose *event, desk *d)
{
    ENTER;
    DBG("d->no=%d\n", d->no);

    if (d->dirty) {
        pager_priv *pg = d->pg;
        task *t;
        int j;

        d->dirty = 0;
        desk_clear_pixmap(d);   /* fill with wallpaper or solid colour */
        /* draw all tracked windows in stacking order */
        for (j = 0; j < pg->winnum; j++) {
            if (!(t = g_hash_table_lookup(pg->htable, &pg->wins[j])))
                continue;
            task_update_pix(t, d);
        }
    }
    /* blit the prepared backing pixmap to the screen */
    gdk_draw_drawable(widget->window,
          widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
          d->pix,
          event->area.x, event->area.y,
          event->area.x, event->area.y,
          event->area.width, event->area.height);
    RET(FALSE);
}


/*
 * desk_configure_event -- "configure_event" handler: resize backing pixmaps.
 *
 * Called when the drawing area is first realised or resized.  Recreates
 * d->pix and d->gpix at the new size, redraws the wallpaper, and recomputes
 * scale factors.
 *
 * Parameters:
 *   widget - the GtkDrawingArea.
 *   event  - configure event (not used directly; dimensions come from allocation).
 *   d      - desk being configured.
 *
 * Returns: FALSE (allow further handlers).
 *
 * Scale factors: scalew = w / screen_w, scaleh = h / screen_h.
 *   (BUG-006 was: these were swapped; now corrected.)
 */
static gint
desk_configure_event (GtkWidget *widget, GdkEventConfigure *event, desk *d)
{
    int w, h;

    ENTER;
    w = widget->allocation.width;
    h = widget->allocation.height;

    DBG("d->no=%d %dx%d %dx%d\n", d->no, w, h, d->pg->daw, d->pg->dah);
    if (d->pix)
        g_object_unref(d->pix);
    if (d->gpix)
        g_object_unref(d->gpix);
    d->pix = gdk_pixmap_new(widget->window, w, h, -1);
    if (d->pg->wallpaper) {
        d->gpix = gdk_pixmap_new(widget->window, w, h, -1);
        desk_draw_bg(d->pg, d);
    }
    d->scalew = (gfloat)w / (gfloat)gdk_screen_width();
    d->scaleh = (gfloat)h / (gfloat)gdk_screen_height();
    desk_set_dirty(d);
    RET(FALSE);
}

/*
 * desk_button_press_event -- "button_press_event" handler.
 *
 * Left-click (button 1 or 2 or any non-Ctrl-RMB) sends
 * _NET_CURRENT_DESKTOP to switch to this desktop.
 * Ctrl+RMB is passed through for the panel's own menu.
 */
static gint
desk_button_press_event(GtkWidget * widget, GdkEventButton * event, desk *d)
{
    ENTER;
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
          && event->state & GDK_CONTROL_MASK) {
        RET(FALSE);   /* let panel handle Ctrl+RMB */
    }
    DBG("s=%d\n", d->no);
    Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, d->no, 0, 0, 0, 0);
    RET(TRUE);
}

/*
 * desk_new -- allocate and initialise a desk struct and its GtkDrawingArea.
 *
 * Creates the drawing area widget, packs it into pg->box, connects
 * expose/configure/button-press event handlers, and shows the widget.
 *
 * Parameters:
 *   pg - pager_priv instance.
 *   i  - desktop index (must be < pg->desknum).
 */
static void
desk_new(pager_priv *pg, int i)
{
    desk *d;

    ENTER;
    g_assert(i < pg->desknum);
    d = pg->desks[i] = g_new0(desk, 1);
    d->pg = pg;
    d->pix = NULL;
    d->dirty = 0;
    d->first = 1;
    d->no = i;

    d->da = gtk_drawing_area_new();
    gtk_widget_set_size_request(d->da, pg->daw, pg->dah);
    gtk_box_pack_start(GTK_BOX(pg->box), d->da, TRUE, TRUE, 0);
    gtk_widget_add_events (d->da, GDK_EXPOSURE_MASK
          | GDK_BUTTON_PRESS_MASK
          | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect (G_OBJECT (d->da), "expose_event",
          (GCallback) desk_expose_event, (gpointer)d);
    g_signal_connect (G_OBJECT (d->da), "configure_event",
          (GCallback) desk_configure_event, (gpointer)d);
    g_signal_connect (G_OBJECT (d->da), "button_press_event",
         (GCallback) desk_button_press_event, (gpointer)d);
    gtk_widget_show_all(d->da);
    RET();
}

/*
 * desk_free -- destroy and free a desk struct.
 *
 * Unrefs both GdkPixmaps, destroys the drawing area widget, and g_free's
 * the desk struct itself.
 *
 * Parameters:
 *   pg - pager_priv instance.
 *   i  - desktop index to free.
 */
static void
desk_free(pager_priv *pg, int i)
{
    desk *d;

    ENTER;
    d = pg->desks[i];
    DBG("i=%d d->no=%d d->da=%p d->pix=%p\n",
          i, d->no, d->da, d->pix);
    if (d->pix)
        g_object_unref(d->pix);
    if (d->gpix)
        g_object_unref(d->gpix);
    gtk_widget_destroy(d->da);
    g_free(d);
    RET();
}


/*****************************************************************
 * Icon helpers — ripped from taskbar.c                         *
 *****************************************************************/

/*
 * get_cmap -- get (and ref) the colormap for a pixmap, handling edge cases.
 *
 * Returns: the colormap (caller must g_object_unref), or NULL for 1-bit depth.
 */
static GdkColormap*
get_cmap (GdkPixmap *pixmap)
{
  GdkColormap *cmap;

  ENTER;
  cmap = gdk_drawable_get_colormap (pixmap);
  if (cmap)
    g_object_ref (G_OBJECT (cmap));

  if (cmap == NULL)
    {
      if (gdk_drawable_get_depth (pixmap) == 1)
        {
          cmap = NULL;   /* 1-bit bitmaps don't need a colormap */
        }
      else
        {
          /* use the screen system colormap as fallback */
          GdkScreen *screen = gdk_drawable_get_screen (GDK_DRAWABLE (pixmap));
          cmap = gdk_screen_get_system_colormap (screen);
          g_object_ref (G_OBJECT (cmap));
        }
    }

  /* ensure visual depth matches drawable depth */
  if (cmap &&
      (gdk_colormap_get_visual (cmap)->depth !=
       gdk_drawable_get_depth (pixmap)))
    cmap = NULL;

  RET(cmap);
}

/*
 * _wnck_gdk_pixbuf_get_from_pixmap -- wrap gdk_pixbuf_get_from_drawable.
 *
 * Looks up the GDK wrapper for @xpixmap (if already tracked), or creates
 * a foreign pixmap wrapper.  Gets the colormap, fetches the pixbuf, cleans up.
 *
 * Returns: GdkPixbuf* (caller owns reference) or NULL on failure.
 */
static GdkPixbuf*
_wnck_gdk_pixbuf_get_from_pixmap (GdkPixbuf   *dest,
                                  Pixmap       xpixmap,
                                  int          src_x,
                                  int          src_y,
                                  int          dest_x,
                                  int          dest_y,
                                  int          width,
                                  int          height)
{
    GdkDrawable *drawable;
    GdkPixbuf *retval;
    GdkColormap *cmap;

    ENTER;
    retval = NULL;

    drawable = gdk_xid_table_lookup (xpixmap);

    if (drawable)
        g_object_ref (G_OBJECT (drawable));
    else
        drawable = gdk_pixmap_foreign_new (xpixmap);

    cmap = get_cmap (drawable);

    /* GTK 2.0.2 workaround: gdk_drawable_get_size may not set -1 correctly */
    if (width < 0)
        gdk_drawable_get_size (drawable, &width, NULL);
    if (height < 0)
        gdk_drawable_get_size (drawable, NULL, &height);

    retval = gdk_pixbuf_get_from_drawable (dest,
          drawable,
          cmap,
          src_x, src_y,
          dest_x, dest_y,
          width, height);

    if (cmap)
        g_object_unref (G_OBJECT (cmap));
    g_object_unref (G_OBJECT (drawable));

    RET(retval);
}

/*
 * apply_mask -- apply a 1-bit mask pixbuf as the alpha channel of a pixbuf.
 *
 * Creates a new RGBA pixbuf from @pixbuf, then sets each pixel's alpha
 * to 0 (transparent) or 255 (opaque) based on the corresponding mask pixel.
 *
 * Returns: new GdkPixbuf with alpha channel (caller owns reference).
 */
static GdkPixbuf*
apply_mask (GdkPixbuf *pixbuf,
            GdkPixbuf *mask)
{
  int w, h;
  int i, j;
  GdkPixbuf *with_alpha;
  guchar *src;
  guchar *dest;
  int src_stride;
  int dest_stride;

  ENTER;
  w = MIN (gdk_pixbuf_get_width (mask), gdk_pixbuf_get_width (pixbuf));
  h = MIN (gdk_pixbuf_get_height (mask), gdk_pixbuf_get_height (pixbuf));

  with_alpha = gdk_pixbuf_add_alpha (pixbuf, FALSE, 0, 0, 0);

  dest = gdk_pixbuf_get_pixels (with_alpha);
  src = gdk_pixbuf_get_pixels (mask);

  dest_stride = gdk_pixbuf_get_rowstride (with_alpha);
  src_stride = gdk_pixbuf_get_rowstride (mask);

  i = 0;
  while (i < h)
    {
      j = 0;
      while (j < w)
        {
          guchar *s = src + i * src_stride + j * 3;
          guchar *d = dest + i * dest_stride + j * 4;

          /* mask pixel: s[0]==s[1]==s[2]; 0 = transparent, non-zero = opaque */
          if (s[0] == 0)
            d[3] = 0;   /* transparent */
          else
            d[3] = 255; /* opaque */

          ++j;
        }

      ++i;
    }

  RET(with_alpha);
}

/*
 * free_pixels -- GdkPixbuf destroy callback that frees the pixel buffer.
 */
static void
free_pixels (guchar *pixels, gpointer data)
{
    ENTER;
    g_free (pixels);
    RET();
}

/*
 * argbdata_to_pixdata -- convert _NET_WM_ICON ARGB data to GdkPixbuf RGBA.
 *
 * _NET_WM_ICON stores pixels as 32-bit ARGB (alpha in high byte).
 * GdkPixbuf expects 32-bit RGBA (alpha in low byte).
 *
 * Parameters:
 *   argb_data - raw ARGB pixel array from _NET_WM_ICON.
 *   len       - number of pixels.
 *
 * Returns: newly allocated RGBA byte array (caller owns; freed by free_pixels).
 */
static guchar *
argbdata_to_pixdata (gulong *argb_data, int len)
{
    guchar *p, *ret;
    int i;

    ENTER;
    ret = p = g_new (guchar, len * 4);
    if (!ret)
        RET(NULL);
    /* convert each ARGB pixel to RGBA */
    i = 0;
    while (i < len) {
        guint32 argb;
        guint32 rgba;

        argb = argb_data[i];
        rgba = (argb << 8) | (argb >> 24);   /* rotate alpha from top to bottom */

        *p = rgba >> 24;        /* R */
        ++p;
        *p = (rgba >> 16) & 0xff;  /* G */
        ++p;
        *p = (rgba >> 8) & 0xff;   /* B */
        ++p;
        *p = rgba & 0xff;          /* A */
        ++p;

        ++i;
    }
    RET(ret);
}

/*
 * get_netwm_icon -- fetch _NET_WM_ICON from a window and scale to iw×ih.
 *
 * The property data format: [width][height][ARGB pixels…].
 * Validates that the icon is at least 16×16 and no larger than 256×256.
 * If width != iw or height != ih, scales with GDK_INTERP_HYPER.
 *
 * Parameters:
 *   tkwin - X11 Window ID.
 *   iw,ih - desired output dimensions.
 *
 * Returns: GdkPixbuf* (caller owns reference) or NULL if unavailable.
 */
static GdkPixbuf *
get_netwm_icon(Window tkwin, int iw, int ih)
{
    gulong *data;
    GdkPixbuf *ret = NULL;
    int n;
    guchar *p;
    GdkPixbuf *src;
    int w, h;

    ENTER;
    data = get_xaproperty(tkwin, a_NET_WM_ICON, XA_CARDINAL, &n);
    if (!data)
        RET(NULL);

    /* (dead code blocks removed from icon-selection loop) */

    /* validate minimum size */
    if (n < (16 * 16 + 1 + 1)) {
        ERR("win %lx: icon is too small or broken (size=%d)\n", tkwin, n);
        goto out;
    }
    w = data[0];
    h = data[1];
    /* validate sensible size range */
    if (w < 16 || w > 256 || h < 16 || h > 256) {
        ERR("win %lx: icon size (%d, %d) is not in 64-256 range\n",
            tkwin, w, h);
        goto out;
    }

    DBG("orig  %dx%d dest %dx%d\n", w, h, iw, ih);
    p = argbdata_to_pixdata(data + 2, w * h);
    if (!p)
        goto out;
    src = gdk_pixbuf_new_from_data (p, GDK_COLORSPACE_RGB, TRUE,
        8, w, h, w * 4, free_pixels, NULL);
    if (src == NULL)
        goto out;
    ret = src;
    if (w != iw || h != ih) {
        ret = gdk_pixbuf_scale_simple(src, iw, ih, GDK_INTERP_HYPER);
        g_object_unref(src);
    }

out:
    XFree(data);
    RET(ret);
}

/*
 * get_wm_icon -- fetch icon from WM_HINTS IconPixmapHint and scale to iw×ih.
 *
 * Falls back to the classic WM_HINTS icon pixmap (and optional mask) when
 * _NET_WM_ICON is not available.  Applies the mask as alpha channel if present.
 *
 * Returns: GdkPixbuf* (caller owns reference) or NULL if unavailable.
 */
static GdkPixbuf*
get_wm_icon(Window tkwin, int iw, int ih)
{
    XWMHints *hints;
    Pixmap xpixmap = None, xmask = None;
    Window win;
    unsigned int w, h;
    int sd;
    GdkPixbuf *ret, *masked, *pixmap, *mask = NULL;

    ENTER;
    hints = XGetWMHints(GDK_DISPLAY(), tkwin);
    DBG("\nwm_hints %s\n", hints ? "ok" : "failed");
    if (!hints)
        RET(NULL);

    if ((hints->flags & IconPixmapHint))
        xpixmap = hints->icon_pixmap;
    if ((hints->flags & IconMaskHint))
        xmask = hints->icon_mask;
    DBG("flag=%ld xpixmap=%lx flag=%ld xmask=%lx\n", (hints->flags & IconPixmapHint), xpixmap,
         (hints->flags & IconMaskHint),  xmask);
    XFree(hints);
    if (xpixmap == None)
        RET(NULL);

    if (!XGetGeometry (GDK_DISPLAY(), xpixmap, &win, &sd, &sd, &w, &h,
              (guint *)&sd, (guint *)&sd)) {
        DBG("XGetGeometry failed for %x pixmap\n", (unsigned int)xpixmap);
        RET(NULL);
    }
    DBG("tkwin=%x icon pixmap w=%d h=%d\n", tkwin, w, h);
    pixmap = _wnck_gdk_pixbuf_get_from_pixmap (NULL, xpixmap, 0, 0, 0, 0, w, h);
    if (!pixmap)
        RET(NULL);
    /* apply mask as alpha channel if mask pixmap is available */
    if (xmask != None && XGetGeometry (GDK_DISPLAY(), xmask,
              &win, &sd, &sd, &w, &h, (guint *)&sd, (guint *)&sd)) {
        mask = _wnck_gdk_pixbuf_get_from_pixmap (NULL, xmask, 0, 0, 0, 0, w, h);

        if (mask) {
            masked = apply_mask (pixmap, mask);
            g_object_unref (G_OBJECT (pixmap));
            g_object_unref (G_OBJECT (mask));
            pixmap = masked;
        }
    }
    if (!pixmap)
        RET(NULL);
    ret = gdk_pixbuf_scale_simple (pixmap, iw, ih, GDK_INTERP_TILES);
    g_object_unref(pixmap);

    RET(ret);
}


/*****************************************************************
 * Netwm/WM Interclient Communication                            *
 *****************************************************************/

/*
 * do_net_active_window -- "active_window" FbEv handler.
 *
 * Fetches _NET_ACTIVE_WINDOW, looks up the corresponding task,
 * and marks the old and new focused task's desks dirty for redraw.
 */
static void
do_net_active_window(FbEv *ev, pager_priv *p)
{
    Window *fwin;
    task *t;

    ENTER;
    fwin = get_xaproperty(GDK_ROOT_WINDOW(), a_NET_ACTIVE_WINDOW, XA_WINDOW, 0);
    DBG("win=%lx\n", fwin ? *fwin : 0);
    if (fwin) {
        t = g_hash_table_lookup(p->htable, fwin);
        if (t != p->focusedtask) {
            if (p->focusedtask)
                desk_set_dirty_by_win(p, p->focusedtask);   /* redraw old focus */
            p->focusedtask = t;
            if (t)
                desk_set_dirty_by_win(p, t);                /* redraw new focus */
        }
        XFree(fwin);
    } else {
        if (p->focusedtask) {
            desk_set_dirty_by_win(p, p->focusedtask);
            p->focusedtask = NULL;
        }
    }
    RET();
}

/*
 * do_net_current_desktop -- "current_desktop" FbEv handler.
 *
 * Updates pg->curdesk, changes the widget state on the old and new
 * current desktops (affecting their border colour), and marks them dirty.
 */
static void
do_net_current_desktop(FbEv *ev, pager_priv *pg)
{
    ENTER;
    desk_set_dirty(pg->desks[pg->curdesk]);
    gtk_widget_set_state(pg->desks[pg->curdesk]->da, GTK_STATE_NORMAL);
    pg->curdesk =  get_net_current_desktop ();
    if (pg->curdesk >= pg->desknum)
        pg->curdesk = 0;
    desk_set_dirty(pg->desks[pg->curdesk]);
    gtk_widget_set_state(pg->desks[pg->curdesk]->da, GTK_STATE_SELECTED);
    RET();
}


/*
 * do_net_client_list_stacking -- "client_list_stacking" FbEv handler.
 *
 * Fetches _NET_CLIENT_LIST_STACKING, reconciles it with the hash table:
 *   - Existing tasks: increment refcount, update stacking order.
 *   - New tasks: allocate, populate, subscribe PropertyChangeMask, insert.
 * Then removes stale tasks (refcount still 0) via task_remove_stale.
 */
static void
do_net_client_list_stacking(FbEv *ev, pager_priv *p)
{
    int i;
    task *t;

    ENTER;
    if (p->wins)
        XFree(p->wins);
    p->wins = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CLIENT_LIST_STACKING,
          XA_WINDOW, &p->winnum);
    if (!p->wins || !p->winnum)
        RET();

    /* refresh existing tasks and add new ones */
    for (i = 0; i < p->winnum; i++) {
        if ((t = g_hash_table_lookup(p->htable, &p->wins[i]))) {
            t->refcount++;   /* bump so task_remove_stale won't remove it */
            if (t->stacking != i) {
                t->stacking = i;
                desk_set_dirty_by_win(p, t);   /* stacking order changed */
            }
        } else {
            /* new window — allocate and populate a task */
            t = g_new0(task, 1);
            t->refcount++;
            t->win = p->wins[i];
            if (!FBPANEL_WIN(t->win))
                XSelectInput (GDK_DISPLAY(), t->win, PropertyChangeMask | StructureNotifyMask);
            t->desktop = get_net_wm_desktop(t->win);
            get_net_wm_state(t->win, &t->nws);
            get_net_wm_window_type(t->win, &t->nwwt);
            task_get_sizepos(t);
            t->pixbuf = get_netwm_icon(t->win, 16, 16);
            t->using_netwm_icon = (t->pixbuf != NULL);
            if (!t->using_netwm_icon) {
                t->pixbuf = get_wm_icon(t->win, 16, 16);
            }
            g_hash_table_insert(p->htable, &t->win, t);
            DBG("add %lx\n", t->win);
            desk_set_dirty_by_win(p, t);
        }
    }
    /* remove windows that are no longer in the stacking list */
    g_hash_table_foreach_remove(p->htable, (GHRFunc) task_remove_stale, (gpointer)p);
    RET();
}


/*****************************************************************
 * Pager Functions                                               *
 *****************************************************************/

/*
 * pager_configurenotify -- handle ConfigureNotify for client windows.
 *
 * When a tracked window moves or resizes, re-fetches its geometry and
 * marks the affected desk dirty.
 */
static void
pager_configurenotify(pager_priv *p, XEvent *ev)
{
    Window win = ev->xconfigure.window;
    task *t;

    ENTER;

    if (!(t = g_hash_table_lookup(p->htable, &win)))
        RET();
    DBG("win=0x%lx\n", win);
    task_get_sizepos(t);
    desk_set_dirty_by_win(p, t);
    RET();
}

/*
 * pager_propertynotify -- handle PropertyNotify for client windows.
 *
 * Watches _NET_WM_STATE (shade/skip_pager changes) and _NET_WM_DESKTOP
 * (window moved to another desktop) on non-root windows.
 * Ignores root-window property changes (handled by FbEv signals).
 */
static void
pager_propertynotify(pager_priv *p, XEvent *ev)
{
    Atom at = ev->xproperty.atom;
    Window win = ev->xproperty.window;
    task *t;


    ENTER;
    if ((win == GDK_ROOT_WINDOW()) || !(t = g_hash_table_lookup(p->htable, &win)))
        RET();

    DBG("window=0x%lx\n", t->win);
    if (at == a_NET_WM_STATE) {
        DBG("event=NET_WM_STATE\n");
        get_net_wm_state(t->win, &t->nws);
    } else if (at == a_NET_WM_DESKTOP) {
        DBG("event=NET_WM_DESKTOP\n");
        desk_set_dirty_by_win(p, t);   /* clean up old desktop first */
        t->desktop = get_net_wm_desktop(t->win);
    } else {
        RET();
    }
    desk_set_dirty_by_win(p, t);
    RET();
}

/*
 * pager_event_filter -- GDK root-window event filter for client X events.
 *
 * Dispatches PropertyNotify (window property changes on client windows)
 * and ConfigureNotify (window geometry changes) to the appropriate handlers.
 *
 * Returns: GDK_FILTER_CONTINUE (always; we don't consume any events).
 */
static GdkFilterReturn
pager_event_filter( XEvent *xev, GdkEvent *event, pager_priv *pg)
{
    ENTER;
    if (xev->type == PropertyNotify )
        pager_propertynotify(pg, xev);
    else if (xev->type == ConfigureNotify )
        pager_configurenotify(pg, xev);
    RET(GDK_FILTER_CONTINUE);
}

/*
 * pager_bg_changed -- FbBg "changed" signal handler.
 *
 * Called when the root window background pixmap changes.  Redraws the
 * wallpaper thumbnail for all desks.
 */
static void
pager_bg_changed(FbBg *bg, pager_priv *pg)
{
    int i;

    ENTER;
    for (i = 0; i < pg->desknum; i++) {
        desk *d = pg->desks[i];
        desk_draw_bg(pg, d);
        desk_set_dirty(d);
    }
    RET();
}


/*
 * pager_rebuild_all -- "number_of_desktops" FbEv handler; also called at init.
 *
 * Reads the new desktop count from the WM, adjusts the desk array
 * (adding new desks or freeing removed ones), clears the task hash
 * table, and refreshes current_desktop and client_list_stacking.
 * Does nothing if the desktop count did not change.
 *
 * Parameters:
 *   ev  - FbEv (may be NULL when called directly).
 *   pg  - pager_priv instance.
 */
static void
pager_rebuild_all(FbEv *ev, pager_priv *pg)
{
    int desknum, dif, i;
    int curdesk G_GNUC_UNUSED;

    ENTER;
    desknum = pg->desknum;
    curdesk = pg->curdesk;

    pg->desknum = get_net_number_of_desktops();
    if (pg->desknum < 1)
        pg->desknum = 1;
    else if (pg->desknum > MAX_DESK_NUM) {
        pg->desknum = MAX_DESK_NUM;
        ERR("pager: max number of supported desks is %d\n", MAX_DESK_NUM);
    }
    pg->curdesk = get_net_current_desktop();
    if (pg->curdesk >= pg->desknum)
        pg->curdesk = 0;
    DBG("desknum=%d curdesk=%d\n", desknum, curdesk);
    DBG("pg->desknum=%d pg->curdesk=%d\n", pg->desknum, pg->curdesk);
    dif = pg->desknum - desknum;

    if (dif == 0)
        RET();   /* no change; nothing to do */

    if (dif < 0) {
        /* desktops were removed — free the excess desk structs */
        for (i = pg->desknum; i < desknum; i++)
            desk_free(pg, i);
    } else {
        /* desktops were added — allocate new desk structs */
        for (i = desknum; i < pg->desknum; i++)
            desk_new(pg, i);
    }
    /* refresh all task data after desktop count change */
    g_hash_table_foreach_remove(pg->htable, (GHRFunc) task_remove_all, (gpointer)pg);
    do_net_current_desktop(NULL, pg);
    do_net_client_list_stacking(NULL, pg);

    RET();
}

/* 1-pixel border inside the pwid container */
#define BORDER 1

/*
 * pager_constructor -- initialise the pager plugin.
 *
 * Creates the thumbnail grid (one GtkDrawingArea per desktop), subscribes
 * to FbEv signals and installs a GDK event filter for client windows.
 * If wallpaper is enabled, acquires an FbBg reference and connects its
 * "changed" signal.
 *
 * Parameters:
 *   plug - plugin_instance allocated by the panel framework.
 *
 * Returns: 1 (always succeeds).
 */
static int
pager_constructor(plugin_instance *plug)
{
    pager_priv *pg;

    ENTER;
    pg = (pager_priv *) plug;

#ifdef EXTRA_DEBUG
    cp = pg;
    signal(SIGUSR2, sig_usr);
#endif

    pg->htable = g_hash_table_new (g_int_hash, g_int_equal);
    pg->box = plug->panel->my_box_new(TRUE, 1);
    gtk_container_set_border_width (GTK_CONTAINER (pg->box), 0);
    gtk_widget_show(pg->box);

    gtk_bgbox_set_background(plug->pwid, BG_STYLE, 0, 0);
    gtk_container_set_border_width (GTK_CONTAINER (plug->pwid), BORDER);
    gtk_container_add(GTK_CONTAINER(plug->pwid), pg->box);

    /* compute thumbnail aspect ratio from screen dimensions */
    pg->ratio = (gfloat)gdk_screen_width() / (gfloat)gdk_screen_height();
    if (plug->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        /* horizontal panel: thumbnail height = panel height - border */
        pg->dah = plug->panel->ah - 2 * BORDER;
        pg->daw = (gfloat) pg->dah * pg->ratio;
    } else {
        /* vertical panel: thumbnail width = panel width - border */
        pg->daw = plug->panel->aw - 2 * BORDER;
        pg->dah = (gfloat) pg->daw / pg->ratio;
    }
    pg->wallpaper = 1;   /* wallpaper enabled by default */
    XCG(plug->xc, "showwallpaper", &pg->wallpaper, enum, bool_enum);
    if (pg->wallpaper) {
        pg->fbbg = fb_bg_get_for_display();
        DBG("get fbbg %p\n", pg->fbbg);
        g_signal_connect(G_OBJECT(pg->fbbg), "changed",
            G_CALLBACK(pager_bg_changed), pg);
    }

    /* default application icon (used when no window icon is available) */
    pg->gen_pixbuf = gdk_pixbuf_new_from_xpm_data((const char **)icon_xpm);
    /* gen_pixbuf is g_object_unref'd in pager_destructor (BUG-007 fix) */

    pager_rebuild_all(fbev, pg);   /* initial desktop setup */

    /* install raw X11 event filter for client-window property/configure events */
    gdk_window_add_filter(NULL, (GdkFilterFunc)pager_event_filter, pg );

    g_signal_connect (G_OBJECT (fbev), "current_desktop",
          G_CALLBACK (do_net_current_desktop), (gpointer) pg);
    g_signal_connect (G_OBJECT (fbev), "active_window",
          G_CALLBACK (do_net_active_window), (gpointer) pg);
    g_signal_connect (G_OBJECT (fbev), "number_of_desktops",
          G_CALLBACK (pager_rebuild_all), (gpointer) pg);
    g_signal_connect (G_OBJECT (fbev), "client_list_stacking",
          G_CALLBACK (do_net_client_list_stacking), (gpointer) pg);
    RET(1);
}

/*
 * pager_destructor -- clean up all pager plugin resources.
 *
 * Disconnects FbEv signals, removes GDK event filter, frees all desks,
 * clears and destroys the hash table, destroys the box widget,
 * releases gen_pixbuf, disconnects FbBg if wallpaper was enabled,
 * and XFree's the window list.
 */
static void
pager_destructor(plugin_instance *p)
{
    pager_priv *pg = (pager_priv *)p;

    ENTER;
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            do_net_current_desktop, pg);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            do_net_active_window, pg);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            pager_rebuild_all, pg);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            do_net_client_list_stacking, pg);
    gdk_window_remove_filter(NULL, (GdkFilterFunc)pager_event_filter, pg);
    /* free all desktop thumbnails in reverse order */
    while (pg->desknum--) {
        desk_free(pg, pg->desknum);
    }
    /* clear all tracked tasks */
    g_hash_table_foreach_remove(pg->htable, (GHRFunc) task_remove_all,
            (gpointer)pg);
    g_hash_table_destroy(pg->htable);
    gtk_widget_destroy(pg->box);
    if (pg->wallpaper) {
        g_signal_handlers_disconnect_by_func(G_OBJECT (pg->fbbg),
              pager_bg_changed, pg);
        DBG("put fbbg %p\n", pg->fbbg);
        g_object_unref(pg->fbbg);   /* release FbBg reference */
    }
    if (pg->gen_pixbuf)
        g_object_unref(pg->gen_pixbuf);
    if (pg->wins)
        XFree(pg->wins);   /* free window list from _NET_CLIENT_LIST_STACKING */
    RET();
}


static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "pager",
    .name        = "Pager",
    .version     = "1.0",
    .description = "Pager shows thumbnails of your desktops",
    .priv_size   = sizeof(pager_priv),

    .constructor = pager_constructor,
    .destructor  = pager_destructor,
};
/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
