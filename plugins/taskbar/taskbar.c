/*
 * taskbar.c -- fbpanel taskbar plugin.
 *
 * Displays one button per open window, allowing raise/iconify/menu actions.
 * Modified 2006-09-10 by Hong Jen Yee (PCMan) to add XUrgencyHint support.
 *
 * Data structures:
 *   task       — per-window state: button widget, name, icon, focus/flash.
 *   taskbar_priv — plugin state: GHashTable of tasks, GtkBar, config options.
 *
 * Event sources:
 *   FbEv signals: current_desktop, active_window, number_of_desktops,
 *                 client_list, desktop_names.
 *   GDK filter: tb_event_filter() handles PropertyNotify on client windows
 *     (window name, icon, state, type, desktop, urgency changes).
 *
 * Rendering:
 *   Each task has a GtkButton containing an image and (optionally) a label.
 *   The GtkBar widget lays buttons in a grid; taskbar_size_alloc recomputes
 *   the number of rows/columns when the widget is resized.
 *
 * Urgency (XUrgencyHint):
 *   When a window sets the urgency hint, tk_flash_window() starts a timeout
 *   that alternates the button's state between GTK_STATE_SELECTED and
 *   normal, creating a flashing effect.
 *
 * Mouse behaviour:
 *   LMB release: raise (or iconify if already focused).
 *   MMB: toggle shaded.
 *   RMB: popup context menu (Raise / Iconify / Move to workspace / Close).
 *   Scroll up: map+raise; scroll down: iconify.
 *   Drag motion with delay: activate window after DRAG_ACTIVE_DELAY ms.
 *   Ctrl+RMB: pass to panel (suppress matching release).
 *
 * Fixed bugs:
 *   Fixed (BUG-008): taskbar_destructor now disconnects all tb_make_menu
 *     FbEv signal connections ("number_of_desktops", "desktop_names") in
 *     addition to the tb_net_number_of_desktops connection.
 *   Fixed (BUG-009): use_net_active moved from file-scope static into
 *     taskbar_priv so each plugin instance has its own copy.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf-xlib/gdk-pixbuf-xlib.h>
#include <gdk/gdk.h>



#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "data/images/default.xpm"
#include "gtkbar.h"

//#define DEBUGPRN
#include "dbg.h"

struct _taskbar;

/*
 * task -- state for one managed window.
 *
 * tb             - back-pointer to taskbar_priv.
 * win            - X11 Window ID.
 * name           - window title with leading/trailing space " title ".
 * iname          - iconified title "[title]".
 * button         - GtkButton widget representing this task.
 * label          - GtkLabel inside button (only if !icons_only).
 * eb             - unused (reserved for event box).
 * image          - GtkImage showing the task icon.
 * pixbuf         - task icon pixbuf (always non-NULL after tk_build_gui).
 * refcount       - stale-task detection counter.
 * ch             - XClassHint (unused currently).
 * pos_x,width    - not used for layout (GtkBar handles that).
 * desktop        - virtual desktop (0-based; 0xFFFFFFFF = all desktops).
 * nws            - _NET_WM_STATE flags (hidden, skip_taskbar, etc.).
 * nwwt           - _NET_WM_WINDOW_TYPE flags (desktop, dock, splash).
 * flash_timeout  - GLib source ID for urgency flashing; 0 if not flashing.
 * focused        - 1 if this is the currently active window.
 * iconified      - 1 if the window is minimised.
 * urgency        - 1 if the window has XUrgencyHint set.
 * using_netwm_icon - 1 if pixbuf came from _NET_WM_ICON.
 * flash          - 1 if flashing is active.
 * flash_state    - current flash phase (0/1).
 */
typedef struct _task{
    struct _taskbar *tb;
    Window win;
    char *name, *iname;
    GtkWidget *button, *label, *eb;
    GtkWidget *image;
    GdkPixbuf *pixbuf;

    int refcount;
    XClassHint ch;
    int pos_x;
    int width;
    guint desktop;
    net_wm_state nws;
    net_wm_window_type nwwt;
    guint flash_timeout;
    unsigned int focused:1;
    unsigned int iconified:1;
    unsigned int urgency:1;
    unsigned int using_netwm_icon:1;
    unsigned int flash:1;
    unsigned int flash_state:1;
} task;



/*
 * taskbar_priv -- private state for one taskbar plugin instance.
 *
 * plugin          - embedded plugin_instance (MUST be first).
 * wins            - XFree-able window list from _NET_CLIENT_LIST.
 * topxwin         - panel's top-level X11 Window ID (to detect own focus).
 * win_num         - number of entries in wins.
 * task_list       - GHashTable mapping Window → task*.
 * hbox,bar,space  - layout widgets (bar = GtkBar; space unused).
 * menu            - right-click context menu (rebuilt on each desktop-names change).
 * gen_pixbuf      - default application icon (from default.xpm).
 * normal_state    - GTK state for unfocused buttons (GTK_STATE_NORMAL).
 * focused_state   - GTK state for focused button (GTK_STATE_ACTIVE).
 * num_tasks       - total number of tracked tasks.
 * task_width_max  - maximum button width (config; default TASK_WIDTH_MAX).
 * task_height_max - maximum button height (from panel->max_elem_height, capped at TASK_HEIGHT_MAX).
 * accept_skip_pager - if 1, also suppress skip_pager windows.
 * show_iconified  - if 1, show minimised windows.
 * show_mapped     - if 1, show mapped (visible) windows.
 * show_all_desks  - if 1, show windows from all desktops.
 * tooltips        - if 1, set button tooltip to window name.
 * icons_only      - if 1, show only the icon (no label).
 * use_mouse_wheel - if 1, scroll events map/iconify windows.
 * use_urgency_hint - if 1, flash urgent windows.
 * discard_release_event - set after Ctrl+RMB to eat the matching release.
 */
typedef struct _taskbar{
    plugin_instance plugin;
    Window *wins;
    Window topxwin;
    int win_num;
    GHashTable  *task_list;
    GtkWidget *hbox, *bar, *space, *menu;
    GdkPixbuf *gen_pixbuf;
    GtkStateType normal_state;
    GtkStateType focused_state;
    int num_tasks;
    int task_width;
    int vis_task_num;
    int req_width;
    int hbox_width;
    int spacing;
    guint cur_desk;
    task *focused;
    task *ptk;
    task *menutask;
    char **desk_names;
    int desk_namesno;
    int desk_num;
    guint dnd_activate;
    int alloc_no;

    int iconsize;
    int task_width_max;
    int task_height_max;
    int accept_skip_pager;
    int show_iconified;
    int show_mapped;
    int show_all_desks;
    int tooltips;
    int icons_only;
    int use_mouse_wheel;
    int use_urgency_hint;
    int discard_release_event;
    gboolean use_net_active;   /* TRUE if WM supports _NET_ACTIVE_WINDOW */
} taskbar_priv;


/* RC string: removes button focus rings and padding for a flat taskbar look */
static gchar *taskbar_rc = "style 'taskbar-style'\n"
"{\n"
"GtkWidget::focus-line-width = 0\n"
"GtkWidget::focus-padding = 0\n"
"GtkButton::default-border = { 0, 0, 0, 0 }\n"
"GtkButton::default-outside-border = { 0, 0, 0, 0 }\n"
"GtkButton::default_border = { 0, 0, 0, 0 }\n"
"GtkButton::default_outside_border = { 0, 0, 0, 0 }\n"
"}\n"
"widget '*.taskbar.*' style 'taskbar-style'";

/* use_net_active has been moved into taskbar_priv (per-instance field). */

/* Delay before activating a window during drag-over (prevents accidental switch) */
#define DRAG_ACTIVE_DELAY       1000


#define TASK_WIDTH_MAX   200   /* default maximum task button width in pixels */
#define TASK_HEIGHT_MAX  28    /* hard cap on task button height */
#define TASK_PADDING     4     /* unused; kept for reference */
static void tk_display(taskbar_priv *tb, task *tk);
static void tb_propertynotify(taskbar_priv *tb, XEvent *ev);
static GdkFilterReturn tb_event_filter( XEvent *, GdkEvent *, taskbar_priv *);
static void taskbar_destructor(plugin_instance *p);

static gboolean tk_has_urgency( task* tk );

static void tk_flash_window( task *tk );
static void tk_unflash_window( task *tk );
static void tk_raise_window( task *tk, guint32 time );

/*
 * TASK_VISIBLE macro -- simple desktop visibility check.
 *
 * A task is visible on the current desktop if its desktop matches
 * cur_desk or it is sticky (desktop == -1 / 0xFFFFFFFF).
 * Note: task_visible() is a superset that also checks iconified/mapped flags.
 */
#define TASK_VISIBLE(tb, tk) \
 ((tk)->desktop == (tb)->cur_desk || (tk)->desktop == -1 /* 0xFFFFFFFF */ )

/*
 * task_visible -- full visibility check honouring all show_* config flags.
 *
 * Returns non-zero if this task should have a visible button.
 */
static int
task_visible(taskbar_priv *tb, task *tk)
{
    ENTER;
    DBG("%lx: desktop=%d iconified=%d \n", tk->win, tk->desktop, tk->iconified);
    RET( (tb->show_all_desks || tk->desktop == -1
            || (tk->desktop == tb->cur_desk))
        && ((tk->iconified && tb->show_iconified)
            || (!tk->iconified && tb->show_mapped)) );
}

/*
 * accept_net_wm_state -- filter based on _NET_WM_STATE flags.
 *
 * Returns 0 if the window should be excluded (skip_taskbar, or skip_pager
 * when accept_skip_pager is non-zero).
 */
inline static int
accept_net_wm_state(net_wm_state *nws, int accept_skip_pager)
{
    ENTER;
    DBG("accept_skip_pager=%d  skip_taskbar=%d skip_pager=%d\n",
        accept_skip_pager,
        nws->skip_taskbar,
        nws->skip_pager);

    RET(!(nws->skip_taskbar || (accept_skip_pager && nws->skip_pager)));
}

/*
 * accept_net_wm_window_type -- filter based on _NET_WM_WINDOW_TYPE flags.
 *
 * Returns 0 for windows that should never appear in the taskbar
 * (desktop, dock, splash windows).
 */
inline static int
accept_net_wm_window_type(net_wm_window_type *nwwt)
{
    ENTER;
    DBG("desktop=%d dock=%d splash=%d\n",
        nwwt->desktop, nwwt->dock, nwwt->splash);

    RET(!(nwwt->desktop || nwwt->dock || nwwt->splash));
}



/*
 * tk_free_names -- free the name and iname strings for a task.
 *
 * Both strings are allocated together (alloc_no tracks the count).
 * Logs a warning if only one is allocated (inconsistent state).
 */
static void
tk_free_names(task *tk)
{
    ENTER;
    if ((!tk->name) != (!tk->iname)) {
        DBG("tk names partially allocated \ntk->name=%s\ntk->iname %s\n",
                tk->name, tk->iname);
    }
    if (tk->name && tk->iname) {
        g_free(tk->name);
        g_free(tk->iname);
        tk->name = tk->iname = NULL;
        tk->tb->alloc_no--;
    }
    RET();
}

/*
 * tk_get_names -- fetch window title and format name / iname strings.
 *
 * Tries _NET_WM_NAME (UTF-8) first, then falls back to XA_WM_NAME.
 * name  = " title "   (with surrounding spaces for button layout).
 * iname = "[title]"   (iconified display format).
 */
static void
tk_get_names(task *tk)
{
    char *name;

    ENTER;
    tk_free_names(tk);
    name = get_utf8_property(tk->win,  a_NET_WM_NAME);
    DBG("a_NET_WM_NAME:%s\n", name);
    if (!name) {
        name = get_textproperty(tk->win,  XA_WM_NAME);
        DBG("XA_WM_NAME:%s\n", name);
    }
    if (name) {
        tk->name  = g_strdup_printf(" %s ", name);    /* padded for button label */
        tk->iname = g_strdup_printf("[%s]", name);    /* iconified format */
        g_free(name);
        tk->tb->alloc_no++;
    }
    RET();
}

/*
 * tk_set_names -- update the button label to reflect iconified state.
 *
 * Uses tk->iname when iconified, tk->name otherwise.
 * Also updates the tooltip if tooltips are enabled.
 */
static void
tk_set_names(task *tk)
{
    char *name;

    ENTER;
    name = tk->iconified ? tk->iname : tk->name;
    if (!tk->tb->icons_only)
        gtk_label_set_text(GTK_LABEL(tk->label), name);
    if (tk->tb->tooltips)
        gtk_widget_set_tooltip_text(tk->button, tk->name);
    RET();
}



/*
 * find_task -- look up a task by Window ID.
 *
 * Returns: task* or NULL if not found.
 */
static task *
find_task (taskbar_priv * tb, Window win)
{
    ENTER;
    RET(g_hash_table_lookup(tb->task_list, &win));
}


/*
 * del_task -- remove a task from the taskbar.
 *
 * Stops any flash timeout, destroys the button widget, frees names,
 * clears focused pointer, and optionally removes from the hash table.
 *
 * Parameters:
 *   tb   - taskbar_priv.
 *   tk   - task to delete.
 *   hdel - if non-zero, remove from tb->task_list hash table.
 */
static void
del_task (taskbar_priv * tb, task *tk, int hdel)
{
    ENTER;
    DBG("deleting(%d)  %08x %s\n", hdel, tk->win, tk->name);
    if (tk->flash_timeout)
        g_source_remove(tk->flash_timeout);   /* stop urgency flash timer */
    gtk_widget_destroy(tk->button);
    tb->num_tasks--;
    tk_free_names(tk);
    if (tb->focused == tk)
        tb->focused = NULL;
    if (hdel)
        g_hash_table_remove(tb->task_list, &tk->win);
    g_free(tk);
    RET();
}



/*
 * get_cmap -- get (and ref) the colormap for a pixmap.
 *
 * Handles the GTK 2.0.2 bug where depth-1 drawables have no colormap.
 * Returns: colormap (caller must g_object_unref), or NULL.
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

  /* ensure colormap visual depth matches drawable depth */
  if (cmap &&
      (gdk_colormap_get_visual (cmap)->depth !=
       gdk_drawable_get_depth (pixmap)))
    cmap = NULL;

  RET(cmap);
}

/*
 * _wnck_gdk_pixbuf_get_from_pixmap -- wrap gdk_pixbuf_get_from_drawable.
 *
 * Looks up the GDK wrapper for @xpixmap, creates a foreign pixmap wrapper
 * if not already tracked, reads pixels with the correct colormap.
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

    /* GTK 2.0.2 workaround: doesn't populate width/height correctly */
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
 * apply_mask -- apply a 1-bit mask pixbuf as the alpha channel.
 *
 * Creates a new RGBA pixbuf, then for each pixel copies R/G/B from
 * @pixbuf and sets alpha to 255 (opaque) or 0 (transparent) from @mask.
 *
 * Returns: new GdkPixbuf with alpha (caller owns reference).
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

          /* mask pixel s[0]==s[1]==s[2]: 0 = transparent, 255 = opaque */
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
 * free_pixels -- GdkPixbuf destroy callback that g_free's the pixel buffer.
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
 * _NET_WM_ICON stores 32-bit ARGB (alpha in high byte).
 * GdkPixbuf expects 32-bit RGBA (alpha in low byte).
 *
 * Returns: newly allocated byte array (freed by free_pixels callback).
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
    /* convert ARGB → RGBA by rotating the alpha byte */
    i = 0;
    while (i < len) {
        guint32 argb;
        guint32 rgba;

        argb = argb_data[i];
        rgba = (argb << 8) | (argb >> 24);

        *p = rgba >> 24;         /* R */
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
 * get_netwm_icon -- fetch _NET_WM_ICON and scale to iw×ih pixels.
 *
 * Validates size (min 16×16, max 256×256) and converts ARGB → RGBA.
 *
 * Returns: GdkPixbuf* (caller owns reference) or NULL.
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

    /* validate minimum size: need at least w + h + 16*16 longs */
    if (n < (16 * 16 + 1 + 1)) {
        ERR("win %lx: icon is too small or broken (size=%d)\n", tkwin, n);
        goto out;
    }
    w = data[0];
    h = data[1];
    /* reject unreasonably sized icons */
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
 * get_wm_icon -- fetch classic WM_HINTS icon pixmap and scale to iw×ih.
 *
 * Used as fallback when _NET_WM_ICON is not available.
 * Applies the mask pixmap as alpha channel if one is provided.
 *
 * Returns: GdkPixbuf* (caller owns reference) or NULL.
 */
static GdkPixbuf *
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
    DBG("flag=%ld xpixmap=%lx flag=%ld xmask=%lx\n",
        (hints->flags & IconPixmapHint), xpixmap,
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
    /* apply mask as alpha if a mask pixmap was given */
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

/*
 * get_generic_icon -- return a ref to the shared default icon.
 *
 * Always succeeds.  Caller must g_object_unref the returned pixbuf.
 */
inline static GdkPixbuf*
get_generic_icon(taskbar_priv *tb)
{
    ENTER;
    g_object_ref(tb->gen_pixbuf);
    RET(tb->gen_pixbuf);
}

/*
 * tk_update_icon -- refresh the icon for a task.
 *
 * If @a is a_NET_WM_ICON or None, attempts to re-fetch the netwm icon.
 * Falls back to WM_HINTS icon, then to the generic icon.
 * Unrefs the old pixbuf if it changed.
 *
 * Parameters:
 *   tb - taskbar_priv.
 *   tk - task whose icon should be updated.
 *   a  - atom that changed (None = refresh everything).
 */
static void
tk_update_icon (taskbar_priv *tb, task *tk, Atom a)
{
    GdkPixbuf *pixbuf;

    ENTER;
    DBG("%lx: ", tk->win);
    pixbuf = tk->pixbuf;
    if (a == a_NET_WM_ICON || a == None) {
        tk->pixbuf = get_netwm_icon(tk->win, tb->iconsize, tb->iconsize);
        tk->using_netwm_icon = (tk->pixbuf != NULL);
        DBGE("netwm_icon=%d ", tk->using_netwm_icon);
    }
    if (!tk->using_netwm_icon) {
        tk->pixbuf = get_wm_icon(tk->win, tb->iconsize, tb->iconsize);
        DBGE("wm_icon=%d ", (tk->pixbuf != NULL));
    }
    if (!tk->pixbuf) {
        tk->pixbuf = get_generic_icon(tb);   /* always non-NULL */
        DBGE("generic_icon=1");
    }
    if (pixbuf != tk->pixbuf) {
        if (pixbuf)
            g_object_unref(pixbuf);   /* release old pixbuf */
    }
    DBGE(" %dx%d \n", gdk_pixbuf_get_width(tk->pixbuf),
        gdk_pixbuf_get_height(tk->pixbuf));
    RET();
}

/*
 * on_flash_win -- GLib timer callback: toggle flash state and update button colour.
 *
 * Alternates the button between GTK_STATE_SELECTED and normal_state.
 *
 * Returns: TRUE (keep the timer running).
 */
static gboolean
on_flash_win( task *tk )
{
    tk->flash_state = !tk->flash_state;
    gtk_widget_set_state(tk->button,
          tk->flash_state ? GTK_STATE_SELECTED : tk->tb->normal_state);
    gtk_widget_queue_draw(tk->button);
    return TRUE;
}

/*
 * tk_flash_window -- start urgency flashing for a task.
 *
 * Reads the GTK cursor blink interval from settings and starts a timeout
 * that calls on_flash_win() at that rate.  Idempotent: does nothing if
 * already flashing.
 */
static void
tk_flash_window( task *tk )
{
    gint interval;
    tk->flash = 1;
    tk->flash_state = !tk->flash_state;
    if (tk->flash_timeout)
        return;   /* already flashing */
    g_object_get( gtk_widget_get_settings(tk->button),
          "gtk-cursor-blink-time", &interval, NULL );
    tk->flash_timeout = g_timeout_add(interval, (GSourceFunc)on_flash_win, tk);
}

/*
 * tk_unflash_window -- stop urgency flashing and restore button state.
 */
static void
tk_unflash_window( task *tk )
{
    tk->flash = tk->flash_state = 0;
    if (tk->flash_timeout) {
        g_source_remove(tk->flash_timeout);
        tk->flash_timeout = 0;
    }
}

/*
 * tk_raise_window -- raise and focus a window.
 *
 * Switches to the window's desktop if needed.  If the WM supports
 * _NET_ACTIVE_WINDOW, sends a client message; otherwise uses
 * XRaiseWindow + XSetInputFocus.
 *
 * Parameters:
 *   tk   - task to raise.
 *   time - X11 event timestamp for _NET_ACTIVE_WINDOW.
 */
static void
tk_raise_window( task *tk, guint32 time )
{
    if (tk->desktop != -1 && tk->desktop != tk->tb->cur_desk){
        /* switch to the window's desktop first */
        Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, tk->desktop,
            0, 0, 0, 0);
        XSync (gdk_display, False);
    }
    if(tk->tb->use_net_active) {
        Xclimsg(tk->win, a_NET_ACTIVE_WINDOW, 2, time, 0, 0, 0);
    }
    else {
        XRaiseWindow (GDK_DISPLAY(), tk->win);
        XSetInputFocus (GDK_DISPLAY(), tk->win, RevertToNone, CurrentTime);
    }
    DBG("XRaiseWindow %x\n", tk->win);
}

/*
 * tk_callback_leave/enter -- restore button state on pointer enter/leave.
 *
 * GTK changes button state on enter/leave; we override it to use
 * focused_state or normal_state based on keyboard focus.
 */
static void
tk_callback_leave( GtkWidget *widget, task *tk)
{
    ENTER;
    gtk_widget_set_state(widget,
          (tk->focused) ? tk->tb->focused_state : tk->tb->normal_state);
    RET();
}


static void
tk_callback_enter( GtkWidget *widget, task *tk )
{
    ENTER;
    gtk_widget_set_state(widget,
          (tk->focused) ? tk->tb->focused_state : tk->tb->normal_state);
    RET();
}

/*
 * delay_active_win -- GLib timeout callback: raise window after drag delay.
 *
 * Called DRAG_ACTIVE_DELAY ms after a drag-motion event to activate the
 * window being dragged over (so the user can drop onto it).
 */
static gboolean
delay_active_win(task* tk)
{
    tk_raise_window(tk, CurrentTime);
    tk->tb->dnd_activate = 0;
    return FALSE;   /* one-shot */
}

/*
 * tk_callback_drag_motion -- activate window after DRAG_ACTIVE_DELAY ms.
 *
 * Starts the delay timer on the first motion event; subsequent events
 * are ignored until the timer fires.
 */
static gboolean
tk_callback_drag_motion( GtkWidget *widget,
      GdkDragContext *drag_context,
      gint x, gint y,
      guint time, task *tk)
{
    /* prevent excessive motion notification */
    if (!tk->tb->dnd_activate) {
        tk->tb->dnd_activate = g_timeout_add(DRAG_ACTIVE_DELAY,
              (GSourceFunc)delay_active_win, tk);
    }
    gdk_drag_status (drag_context,0,time);
    return TRUE;
}

/*
 * tk_callback_drag_leave -- cancel the drag-activation timer.
 */
static void
tk_callback_drag_leave (GtkWidget *widget,
      GdkDragContext *drag_context,
      guint time, task *tk)
{
    if (tk->tb->dnd_activate) {
        g_source_remove(tk->tb->dnd_activate);
        tk->tb->dnd_activate = 0;
    }
    return;
}

/*
 * tk_callback_scroll_event -- mouse-wheel handler.
 *
 * Scroll up: map and raise the window (un-iconify).
 * Scroll down: iconify the window.
 * Only active when use_mouse_wheel is set.
 */
static gint
tk_callback_scroll_event (GtkWidget *widget, GdkEventScroll *event, task *tk)
{
    ENTER;
    if (event->direction == GDK_SCROLL_UP) {
        GdkWindow *gdkwindow;

        gdkwindow = gdk_xid_table_lookup (tk->win);
        if (gdkwindow)
            gdk_window_show (gdkwindow);
        else
            XMapRaised (GDK_DISPLAY(), tk->win);
        XSetInputFocus (GDK_DISPLAY(), tk->win, RevertToNone, CurrentTime);
        DBG("XMapRaised  %x\n", tk->win);
    } else if (event->direction == GDK_SCROLL_DOWN) {
        DBG("tb->ptk = %x\n", (tk->tb->ptk) ? tk->tb->ptk->win : 0);
        XIconifyWindow (GDK_DISPLAY(), tk->win, DefaultScreen(GDK_DISPLAY()));
        DBG("XIconifyWindow %x\n", tk->win);
    }

    XSync (gdk_display, False);
    RET(TRUE);
}


/*
 * tk_callback_button_press_event -- intercept Ctrl+RMB before release.
 *
 * When Ctrl+RMB is pressed, propagates to the bar (for the panel's
 * context menu) and sets discard_release_event to eat the coming release.
 */
static gboolean
tk_callback_button_press_event(GtkWidget *widget, GdkEventButton *event,
    task *tk)
{
    ENTER;
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
          && event->state & GDK_CONTROL_MASK) {
        tk->tb->discard_release_event = 1;
        gtk_propagate_event(tk->tb->bar, (GdkEvent *)event);
        RET(TRUE);
    }
    RET(FALSE);
}


/*
 * tk_callback_button_release_event -- handle LMB/MMB/RMB release on task button.
 *
 * LMB: if iconified → map/raise; if focused or ptk → iconify; else → raise.
 * MMB: toggle shaded state via _NET_WM_STATE.
 * RMB: show context menu.
 * Ctrl+RMB release: discard.
 * Pointer must be inside the button (GTK_BUTTON::in_button check).
 */
static gboolean
tk_callback_button_release_event(GtkWidget *widget, GdkEventButton *event,
    task *tk)
{
    ENTER;

    if (event->type == GDK_BUTTON_RELEASE && tk->tb->discard_release_event) {
        tk->tb->discard_release_event = 0;
        RET(TRUE);   /* eat the Ctrl+RMB release */
    }
    if ((event->type != GDK_BUTTON_RELEASE) || (!GTK_BUTTON(widget)->in_button))
        RET(FALSE);
    DBG("win=%x\n", tk->win);
    if (event->button == 1) {
        if (tk->iconified)    {
            /* un-iconify (map + raise) the window */
            if(tk->tb->use_net_active) {
                Xclimsg(tk->win, a_NET_ACTIVE_WINDOW, 2, event->time, 0, 0, 0);
            } else {
                GdkWindow *gdkwindow;

                gdkwindow = gdk_xid_table_lookup (tk->win);
                if (gdkwindow)
                    gdk_window_show (gdkwindow);
                else
                    XMapRaised (GDK_DISPLAY(), tk->win);
                XSync (GDK_DISPLAY(), False);
                DBG("XMapRaised  %x\n", tk->win);
            }
        } else {
            DBG("tb->ptk = %x\n", (tk->tb->ptk) ? tk->tb->ptk->win : 0);
            if (tk->focused || tk == tk->tb->ptk) {
                /* clicking the focused window iconifies it */
                XIconifyWindow (GDK_DISPLAY(), tk->win,
                    DefaultScreen(GDK_DISPLAY()));
                DBG("XIconifyWindow %x\n", tk->win);
            } else {
                /* clicking an unfocused window raises it */
                tk_raise_window( tk, event->time );
            }
        }
    } else if (event->button == 2) {
        /* MMB: toggle shaded state */
        Xclimsg(tk->win, a_NET_WM_STATE,
            2 /*a_NET_WM_STATE_TOGGLE*/,
            a_NET_WM_STATE_SHADED,
            0, 0, 0);
    } else if (event->button == 3) {
        /* RMB: show the context menu */
        tk->tb->menutask = tk;
        gtk_menu_popup (GTK_MENU (tk->tb->menu), NULL, NULL,
            (GtkMenuPositionFunc)menu_pos, widget, event->button, event->time);

    }
    gtk_button_released(GTK_BUTTON(widget));
    XSync (gdk_display, False);
    RET(TRUE);
}


/*
 * tk_update -- show or hide a task's button based on current visibility rules.
 *
 * Called from tb_display (via g_hash_table_foreach) and directly after
 * focus changes.  Sets button state to focused/normal and shows/hides it.
 */
static void
tk_update(gpointer key, task *tk, taskbar_priv *tb)
{
    ENTER;
    g_assert ((tb != NULL) && (tk != NULL));
    if (task_visible(tb, tk)) {
        gtk_widget_set_state (tk->button,
              (tk->focused) ? tb->focused_state : tb->normal_state);
        gtk_widget_queue_draw(tk->button);
        gtk_widget_show(tk->button);

        if (tb->tooltips) {
            gtk_widget_set_tooltip_text(tk->button, tk->name);
        }
        RET();
    }
    gtk_widget_hide(tk->button);
    RET();
}

/*
 * tk_display -- refresh a single task's button visibility.
 */
static void
tk_display(taskbar_priv *tb, task *tk)
{
    ENTER;
    tk_update(NULL, tk, tb);
    RET();
}

/*
 * tb_display -- refresh all task buttons.
 */
static void
tb_display(taskbar_priv *tb)
{
    ENTER;
    if (tb->wins)
        g_hash_table_foreach(tb->task_list, (GHFunc) tk_update, (gpointer) tb);
    RET();

}

/*
 * tk_build_gui -- create all GTK widgets for one task.
 *
 * Builds: GtkButton → GtkHBox → GtkImage [+ GtkLabel]
 * Connects button event handlers, sets up DnD drag destination,
 * and starts urgency flashing if tk->urgency is set.
 *
 * Parameters:
 *   tb - taskbar_priv.
 *   tk - task for which to build the button.
 */
static void
tk_build_gui(taskbar_priv *tb, task *tk)
{
    GtkWidget *w1;

    ENTER;
    g_assert ((tb != NULL) && (tk != NULL));

    /* subscribe to X11 events for non-panel windows */
    if (!FBPANEL_WIN(tk->win))
        XSelectInput(GDK_DISPLAY(), tk->win,
                PropertyChangeMask | StructureNotifyMask);

    /* create the button */
    tk->button = gtk_button_new();
    gtk_button_set_alignment(GTK_BUTTON(tk->button), 0.5, 0.5);
    gtk_widget_show(tk->button);
    gtk_container_set_border_width(GTK_CONTAINER(tk->button), 0);
    gtk_widget_add_events (tk->button, GDK_BUTTON_RELEASE_MASK
            | GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(tk->button), "button_release_event",
          G_CALLBACK(tk_callback_button_release_event), (gpointer)tk);
    g_signal_connect(G_OBJECT(tk->button), "button_press_event",
           G_CALLBACK(tk_callback_button_press_event), (gpointer)tk);
    g_signal_connect_after (G_OBJECT (tk->button), "leave",
          G_CALLBACK (tk_callback_leave), (gpointer) tk);
    g_signal_connect_after (G_OBJECT (tk->button), "enter",
          G_CALLBACK (tk_callback_enter), (gpointer) tk);
    /* configure drag destination for drag-over window activation */
    gtk_drag_dest_set( tk->button, 0, NULL, 0, 0);
    g_signal_connect (G_OBJECT (tk->button), "drag-motion",
          G_CALLBACK (tk_callback_drag_motion), (gpointer) tk);
    g_signal_connect (G_OBJECT (tk->button), "drag-leave",
          G_CALLBACK (tk_callback_drag_leave), (gpointer) tk);
    if (tb->use_mouse_wheel)
        g_signal_connect_after(G_OBJECT(tk->button), "scroll-event",
              G_CALLBACK(tk_callback_scroll_event), (gpointer)tk);

    /* icon image */
    tk_update_icon(tb, tk, None);
    w1 = tk->image = gtk_image_new_from_pixbuf(tk->pixbuf);
    gtk_misc_set_alignment(GTK_MISC(tk->image), 0.5, 0.5);
    gtk_misc_set_padding(GTK_MISC(tk->image), 0, 0);

    if (!tb->icons_only) {
        /* icon + label layout */
        w1 = gtk_hbox_new(FALSE, 1);
        gtk_container_set_border_width(GTK_CONTAINER(w1), 0);
        gtk_box_pack_start(GTK_BOX(w1), tk->image, FALSE, FALSE, 0);
        tk->label = gtk_label_new(tk->iconified ? tk->iname : tk->name);
        gtk_label_set_ellipsize(GTK_LABEL(tk->label), PANGO_ELLIPSIZE_END);
        gtk_misc_set_alignment(GTK_MISC(tk->label), 0.0, 0.5);
        gtk_misc_set_padding(GTK_MISC(tk->label), 0, 0);
        gtk_box_pack_start(GTK_BOX(w1), tk->label, TRUE, TRUE, 0);
    }

    gtk_container_add (GTK_CONTAINER (tk->button), w1);
    gtk_box_pack_start(GTK_BOX(tb->bar), tk->button, FALSE, TRUE, 0);
    GTK_WIDGET_UNSET_FLAGS (tk->button, GTK_CAN_FOCUS);
    GTK_WIDGET_UNSET_FLAGS (tk->button, GTK_CAN_DEFAULT);

    gtk_widget_show_all(tk->button);
    if (!task_visible(tb, tk)) {
        gtk_widget_hide(tk->button);
    }

    if (tk->urgency) {
        /* start flashing for windows with urgency hint set */
        tk_flash_window(tk);
    }
    RET();
}

/*
 * task_remove_every -- GHashTable foreach-remove callback: remove all tasks.
 *
 * Used in the destructor to clean up all tasks.
 */
static gboolean
task_remove_every(Window *win, task *tk)
{
    ENTER;
    del_task(tk->tb, tk, 0);
    RET(TRUE);
}

/*
 * task_remove_stale -- GHashTable foreach-remove callback.
 *
 * Removes tasks whose refcount has reached 0 (no longer in _NET_CLIENT_LIST).
 * Returns: TRUE to remove, FALSE to keep.
 */
static gboolean
task_remove_stale(Window *win, task *tk, gpointer data)
{
    ENTER;
    if (tk->refcount-- == 0) {
        del_task(tk->tb, tk, 0);
        RET(TRUE);
    }
    RET(FALSE);
}

/*****************************************************
 * handlers for NET actions                          *
 *****************************************************/


/*
 * tb_net_client_list -- "client_list" FbEv handler.
 *
 * Fetches _NET_CLIENT_LIST, creates task structs for new windows
 * (after filtering by nws/nwwt), and removes stale ones.
 * Calls tb_display() to refresh all button visibility.
 */
static void
tb_net_client_list(GtkWidget *widget, taskbar_priv *tb)
{
    int i;
    task *tk;

    ENTER;
    if (tb->wins)
        XFree(tb->wins);
    tb->wins = get_xaproperty (GDK_ROOT_WINDOW(),
        a_NET_CLIENT_LIST, XA_WINDOW, &tb->win_num);
    if (!tb->wins)
        RET();
    for (i = 0; i < tb->win_num; i++) {
        if ((tk = g_hash_table_lookup(tb->task_list, &tb->wins[i]))) {
            tk->refcount++;   /* bump so task_remove_stale won't remove it */
        } else {
            net_wm_window_type nwwt;
            net_wm_state nws;

            get_net_wm_state(tb->wins[i], &nws);
            if (!accept_net_wm_state(&nws, tb->accept_skip_pager))
                continue;
            get_net_wm_window_type(tb->wins[i], &nwwt);
            if (!accept_net_wm_window_type(&nwwt))
                continue;

            tk = g_new0(task, 1);
            tk->refcount = 1;
            tb->num_tasks++;
            tk->win = tb->wins[i];
            tk->tb = tb;
            tk->iconified = nws.hidden;
            tk->desktop = get_net_wm_desktop(tk->win);
            tk->nws = nws;
            tk->nwwt = nwwt;
            if( tb->use_urgency_hint && tk_has_urgency(tk)) {
                tk->urgency = 1;
            }

            tk_build_gui(tb, tk);
            tk_get_names(tk);
            tk_set_names(tk);

            g_hash_table_insert(tb->task_list, &tk->win, tk);
            DBG("adding %08x(%p) %s\n", tk->win,
                FBPANEL_WIN(tk->win), tk->name);
        }
    }

    /* remove windows no longer in _NET_CLIENT_LIST */
    g_hash_table_foreach_remove(tb->task_list, (GHRFunc) task_remove_stale,
        NULL);
    tb_display(tb);
    RET();
}



/*
 * tb_net_current_desktop -- "current_desktop" FbEv handler.
 *
 * Updates cur_desk and refreshes all task button visibility.
 */
static void
tb_net_current_desktop(GtkWidget *widget, taskbar_priv *tb)
{
    ENTER;
    tb->cur_desk = get_net_current_desktop();
    tb_display(tb);
    RET();
}


/*
 * tb_net_number_of_desktops -- "number_of_desktops" FbEv handler.
 *
 * Updates desk_num and refreshes all task button visibility.
 */
static void
tb_net_number_of_desktops(GtkWidget *widget, taskbar_priv *tb)
{
    ENTER;
    tb->desk_num = get_net_number_of_desktops();
    tb_display(tb);
    RET();
}


/*
 * tb_net_active_window -- "active_window" FbEv handler.
 *
 * Fetches _NET_ACTIVE_WINDOW and updates the focused task.
 * Maintains tb->ptk (previously-focused task) for the iconify-on-click logic.
 *
 * Special case: if the newly active window is topxwin (the panel itself),
 * the current task's focus is dropped but ptk is set to remember it.
 */
static void
tb_net_active_window(GtkWidget *widget, taskbar_priv *tb)
{
    Window *f;
    task *ntk, *ctk;
    int drop_old, make_new;

    ENTER;
    g_assert (tb != NULL);
    drop_old = make_new = 0;
    ctk = tb->focused;
    ntk = NULL;
    f = get_xaproperty(GDK_ROOT_WINDOW(), a_NET_ACTIVE_WINDOW, XA_WINDOW, 0);
    DBG("FOCUS=%x\n", f ? *f : 0);
    if (!f) {
        /* no active window — drop focus entirely */
        drop_old = 1;
        tb->ptk = NULL;
    } else {
        if (*f == tb->topxwin) {
            /* panel itself gained focus — remember which task was focused */
            if (ctk) {
                tb->ptk = ctk;
                drop_old = 1;
            }
        } else {
            tb->ptk = NULL;
            ntk = find_task(tb, *f);
            if (ntk != ctk) {
                drop_old = 1;
                make_new = 1;
            }
        }
        XFree(f);
    }
    if (ctk && drop_old) {
        ctk->focused = 0;
        tb->focused = NULL;
        tk_display(tb, ctk);
        DBG("old focus was dropped\n");
    }
    if (ntk && make_new) {
        ntk->focused = 1;
        tb->focused = ntk;
        tk_display(tb, ntk);
        DBG("new focus was set\n");
    }
    RET();
}

/* Backwards-compat define for older Xlib headers */
#ifndef XUrgencyHint
#define XUrgencyHint (1 << 8)
#endif

/*
 * tk_has_urgency -- check whether a window has XUrgencyHint set.
 *
 * Fetches WM_HINTS and checks for XUrgencyHint.  Updates tk->urgency.
 *
 * Returns: non-zero if urgency is set, 0 otherwise.
 */
static gboolean
tk_has_urgency( task* tk )
{
    XWMHints* hints;

    tk->urgency = 0;
    hints = XGetWMHints(GDK_DISPLAY(), tk->win);
    if (hints) {
        if (hints->flags & XUrgencyHint)   /* urgency hint present */
            tk->urgency = 1;
        XFree( hints );
    }
    return tk->urgency;
}

/*
 * tb_propertynotify -- handle PropertyNotify for a client window.
 *
 * Dispatches on the changed atom:
 *   _NET_WM_DESKTOP  → update desktop, refresh display.
 *   XA_WM_NAME       → refresh name/iname strings and label.
 *   XA_WM_HINTS      → refresh icon, check/update urgency flash.
 *   _NET_WM_STATE    → re-check accept, update iconified, refresh name.
 *   _NET_WM_ICON     → refresh icon pixbuf.
 *   _NET_WM_WINDOW_TYPE → re-check accept; remove if now excluded.
 *
 * Root-window property events are ignored (handled via FbEv signals).
 */
static void
tb_propertynotify(taskbar_priv *tb, XEvent *ev)
{
    Atom at;
    Window win;

    ENTER;
    DBG("win=%x\n", ev->xproperty.window);
    at = ev->xproperty.atom;
    win = ev->xproperty.window;
    if (win != GDK_ROOT_WINDOW()) {
        task *tk = find_task(tb, win);

        if (!tk) RET();
        DBG("win=%x\n", ev->xproperty.window);
        if (at == a_NET_WM_DESKTOP) {
            DBG("NET_WM_DESKTOP\n");
            tk->desktop = get_net_wm_desktop(win);
            tb_display(tb);
        } else if (at == XA_WM_NAME) {
            DBG("WM_NAME\n");
            tk_get_names(tk);
            tk_set_names(tk);
        } else if (at == XA_WM_HINTS)   {
            /* some windows set their WM_HINTS icon after mapping */
            DBG("XA_WM_HINTS\n");
            tk_update_icon (tb, tk, XA_WM_HINTS);
            gtk_image_set_from_pixbuf (GTK_IMAGE(tk->image), tk->pixbuf);
            if (tb->use_urgency_hint) {
                if (tk_has_urgency(tk)) {
                    tk_flash_window(tk);
                } else {
                    tk_unflash_window(tk);
                }
            }
        } else if (at == a_NET_WM_STATE) {
            net_wm_state nws;

            DBG("_NET_WM_STATE\n");
            get_net_wm_state(tk->win, &nws);
            if (!accept_net_wm_state(&nws, tb->accept_skip_pager)) {
                del_task(tb, tk, 1);
                tb_display(tb);
            } else {
                tk->iconified = nws.hidden;
                tk_set_names(tk);
            }
        } else if (at == a_NET_WM_ICON) {
            DBG("_NET_WM_ICON\n");
            tk_update_icon (tb, tk, a_NET_WM_ICON);
            gtk_image_set_from_pixbuf (GTK_IMAGE(tk->image), tk->pixbuf);
        } else if (at == a_NET_WM_WINDOW_TYPE) {
            net_wm_window_type nwwt;

            DBG("_NET_WM_WINDOW_TYPE\n");
            get_net_wm_window_type(tk->win, &nwwt);
            if (!accept_net_wm_window_type(&nwwt)) {
                del_task(tb, tk, 1);
                tb_display(tb);
            }
        } else {
            DBG("at = %d\n", at);
        }
    }
    RET();
}

/*
 * tb_event_filter -- GDK event filter for X11 PropertyNotify on client windows.
 *
 * Dispatches to tb_propertynotify for all PropertyNotify events.
 * Root-window events are ignored here (FbEv handles those separately).
 *
 * Returns: GDK_FILTER_CONTINUE (never consumes events).
 */
static GdkFilterReturn
tb_event_filter( XEvent *xev, GdkEvent *event, taskbar_priv *tb)
{

    ENTER;
    g_assert(tb != NULL);
    if (xev->type == PropertyNotify )
        tb_propertynotify(tb, xev);
    RET(GDK_FILTER_CONTINUE);
}

/*
 * menu_close_window -- "activate" handler for the "Close" menu item.
 *
 * Sends WM_DELETE_WINDOW message via Xclimsgwm (polite close request).
 */
static void
menu_close_window(GtkWidget *widget, taskbar_priv *tb)
{
    ENTER;
    DBG("win %x\n", tb->menutask->win);
    XSync (GDK_DISPLAY(), 0);
    Xclimsgwm(tb->menutask->win, a_WM_PROTOCOLS, a_WM_DELETE_WINDOW);
    XSync (GDK_DISPLAY(), 0);
    RET();
}


/*
 * menu_raise_window -- "activate" handler for the "Raise" menu item.
 */
static void
menu_raise_window(GtkWidget *widget, taskbar_priv *tb)
{
    ENTER;
    DBG("win %x\n", tb->menutask->win);
    XMapRaised(GDK_DISPLAY(), tb->menutask->win);
    RET();
}


/*
 * menu_iconify_window -- "activate" handler for the "Iconify" menu item.
 */
static void
menu_iconify_window(GtkWidget *widget, taskbar_priv *tb)
{
    ENTER;
    DBG("win %x\n", tb->menutask->win);
    XIconifyWindow (GDK_DISPLAY(), tb->menutask->win,
        DefaultScreen(GDK_DISPLAY()));
    RET();
}

/*
 * send_to_workspace -- "button_press_event" handler for workspace submenu items.
 *
 * Reads the "num" data from the menu item and sends _NET_WM_DESKTOP to
 * move menutask's window to that desktop.
 */
static void
send_to_workspace(GtkWidget *widget, void *iii, taskbar_priv *tb)
{
    int dst_desktop;

    ENTER;

    dst_desktop = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "num"));
    DBG("win %x -> %d\n",  (unsigned int)tb->menutask->win, dst_desktop);
    Xclimsg(tb->menutask->win, a_NET_WM_DESKTOP, dst_desktop, 0, 0, 0, 0);

    RET();
}

#define ALL_WORKSPACES  0xFFFFFFFF   /* _NET_WM_DESKTOP value for sticky windows */

/*
 * tb_update_desktops_names -- refresh tb->desk_names and desk_namesno.
 *
 * Reads _NET_DESKTOP_NAMES (UTF-8 list) from the root window.
 * g_strfreev's the previous list before replacing.
 */
static void
tb_update_desktops_names(taskbar_priv *tb)
{
    ENTER;
    tb->desk_namesno = get_net_number_of_desktops();
    if (tb->desk_names)
        g_strfreev(tb->desk_names);
    tb->desk_names = get_utf8_property_list(GDK_ROOT_WINDOW(),
        a_NET_DESKTOP_NAMES, &(tb->desk_namesno));
    RET();
}

/*
 * tb_make_menu -- build (or rebuild) the right-click context menu.
 *
 * Called from taskbar_build_gui and whenever "desktop_names" or
 * "number_of_desktops" FbEv signals fire.
 * Destroys the previous menu and creates a new one with:
 *   Raise / Iconify / Move to workspace (submenu) / Close.
 *
 * Parameters:
 *   widget - unused (required by FbEv signal signature).
 *   tb     - taskbar_priv.
 */
static void
tb_make_menu(GtkWidget *widget, taskbar_priv *tb)
{
    GtkWidget *mi, *menu, *submenu;
    gchar *buf;
    int i;

    ENTER;
    menu = gtk_menu_new ();

    mi = gtk_image_menu_item_new_with_label (_("Raise"));
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi),
          gtk_image_new_from_stock(GTK_STOCK_GO_UP, GTK_ICON_SIZE_MENU));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate",
        (GCallback)menu_raise_window, tb);
    gtk_widget_show (mi);

    mi = gtk_image_menu_item_new_with_label (_("Iconify"));
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi),
          gtk_image_new_from_stock(GTK_STOCK_UNDO, GTK_ICON_SIZE_MENU));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate",
        (GCallback)menu_iconify_window, tb);
    gtk_widget_show (mi);

    /* "Move to workspace" submenu */
    tb_update_desktops_names(tb);
    submenu = gtk_menu_new();
    for (i = 0; i < tb->desk_num; i++) {
        buf = g_strdup_printf("%d  %s", i + 1,
            (i < tb->desk_namesno) ? tb->desk_names[i] : "");
        mi = gtk_image_menu_item_new_with_label (buf);
        g_object_set_data(G_OBJECT(mi), "num", GINT_TO_POINTER(i));
        gtk_menu_shell_append (GTK_MENU_SHELL (submenu), mi);
        /* NOTE: uses button_press_event not activate — send_to_workspace gets 3 args */
        g_signal_connect(G_OBJECT(mi), "button_press_event",
            (GCallback)send_to_workspace, tb);
        g_free(buf);
    }
    /* "All workspaces" sticky option */
    mi = gtk_image_menu_item_new_with_label(_("All workspaces"));
    g_object_set_data(G_OBJECT(mi), "num", GINT_TO_POINTER(ALL_WORKSPACES));
    g_signal_connect(mi, "activate",
        (GCallback)send_to_workspace, tb);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), mi);
    gtk_widget_show_all(submenu);

    mi = gtk_image_menu_item_new_with_label(_("Move to workspace"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), submenu);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi),
          gtk_image_new_from_stock(GTK_STOCK_JUMP_TO, GTK_ICON_SIZE_MENU));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    gtk_widget_show (mi);

    mi = gtk_separator_menu_item_new();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    gtk_widget_show (mi);

    /* Close — placed last so it's furthest from the mouse pointer */
    mi = gtk_image_menu_item_new_from_stock(GTK_STOCK_CLOSE, NULL);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate",
        (GCallback)menu_close_window, tb);
    gtk_widget_show (mi);

    if (tb->menu)
        gtk_widget_destroy(tb->menu);   /* destroy the previous menu */
    tb->menu = menu;
}

/*
 * taskbar_size_alloc -- "size-allocate" handler on the GtkAlignment.
 *
 * Recomputes the GtkBar dimension (rows or columns) when the taskbar
 * changes size.
 */
static void
taskbar_size_alloc(GtkWidget *widget, GtkAllocation *a,
    taskbar_priv *tb)
{
    int dim;

    ENTER;
    if (tb->plugin.panel->orientation == GTK_ORIENTATION_HORIZONTAL)
        dim = a->height / tb->task_height_max;
    else
        dim = a->width / tb->task_width_max;
    DBG("width=%d height=%d task_height_max=%d -> dim=%d\n",
        a->width, a->height, tb->task_height_max, dim);
    gtk_bar_set_dimension(GTK_BAR(tb->bar), dim);
    RET();
}

/*
 * taskbar_build_gui -- build the taskbar widget hierarchy.
 *
 * Creates: p->pwid → GtkAlignment → GtkBar (tb->bar)
 * Loads the default icon, installs the GDK event filter, connects FbEv
 * signals, and performs an initial build of the context menu.
 *
 * All FbEv connections (tb_net_number_of_desktops, tb_make_menu) are
 * disconnected in taskbar_destructor (BUG-008 fix).
 */
static void
taskbar_build_gui(plugin_instance *p)
{
    taskbar_priv *tb = (taskbar_priv *) p;
    GtkWidget *ali;

    ENTER;
    /* alignment positions the bar flush left (horizontal) or top (vertical) */
    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL)
        ali = gtk_alignment_new(0.0, 0.5, 0, 0);
    else
        ali = gtk_alignment_new(0.5, 0.0, 0, 0);
    g_signal_connect(G_OBJECT(ali), "size-allocate",
        (GCallback) taskbar_size_alloc, tb);
    gtk_container_set_border_width(GTK_CONTAINER(ali), 0);
    gtk_container_add(GTK_CONTAINER(p->pwid), ali);

    tb->bar = gtk_bar_new(p->panel->orientation, tb->spacing,
        tb->task_height_max, tb->task_width_max);
    gtk_container_set_border_width(GTK_CONTAINER(tb->bar), 0);
    gtk_container_add(GTK_CONTAINER(ali), tb->bar);
    gtk_widget_show_all(ali);

    /* default icon used when a window has no icon of its own */
    tb->gen_pixbuf = gdk_pixbuf_new_from_xpm_data((const char **)icon_xpm);

    /* raw X11 event filter for PropertyNotify on client windows */
    gdk_window_add_filter(NULL, (GdkFilterFunc)tb_event_filter, tb );

    g_signal_connect (G_OBJECT (fbev), "current_desktop",
          G_CALLBACK (tb_net_current_desktop), (gpointer) tb);
    g_signal_connect (G_OBJECT (fbev), "active_window",
          G_CALLBACK (tb_net_active_window), (gpointer) tb);
    g_signal_connect (G_OBJECT (fbev), "number_of_desktops",
          G_CALLBACK (tb_net_number_of_desktops), (gpointer) tb);
    g_signal_connect (G_OBJECT (fbev), "client_list",
          G_CALLBACK (tb_net_client_list), (gpointer) tb);
    g_signal_connect (G_OBJECT (fbev), "desktop_names",
          G_CALLBACK (tb_make_menu), (gpointer) tb);
    /* second connection for menu rebuild (disconnected via tb_make_menu in destructor) */
    g_signal_connect (G_OBJECT (fbev), "number_of_desktops",
          G_CALLBACK (tb_make_menu), (gpointer) tb);

    tb->desk_num = get_net_number_of_desktops();
    tb->cur_desk = get_net_current_desktop();
    tb->focused = NULL;
    tb->menu = NULL;

    tb_make_menu(NULL, tb);   /* initial context menu build */
    gtk_container_set_border_width(GTK_CONTAINER(p->pwid), 0);
    gtk_widget_show_all(tb->bar);
    RET();
}

/*
 * net_active_detect -- check if the WM supports _NET_ACTIVE_WINDOW.
 *
 * Reads _NET_SUPPORTED from the root window and scans for the atom.
 * Sets tb->use_net_active (per-instance; not a shared static).
 */
static void net_active_detect(taskbar_priv *tb)
{
    int nitens;
    Atom *data;

    data = get_xaproperty(GDK_ROOT_WINDOW(), a_NET_SUPPORTED, XA_ATOM, &nitens);
    if (!data)
        return;

    while (nitens > 0)
        if(data[--nitens]==a_NET_ACTIVE_WINDOW) {
            tb->use_net_active = TRUE;
            break;
        }

    XFree(data);
}

/*
 * taskbar_constructor -- initialise the taskbar plugin.
 *
 * Reads config, builds the widget hierarchy, fetches the initial
 * window list, and updates the focused window indicator.
 *
 * Parameters:
 *   p - plugin_instance allocated by the panel framework.
 *
 * Returns: 1 (always succeeds).
 */
int
taskbar_constructor(plugin_instance *p)
{
    taskbar_priv *tb;
    GtkRequisition req;
    xconf *xc = p->xc;

    ENTER;
    tb = (taskbar_priv *) p;
    gtk_rc_parse_string(taskbar_rc);
    get_button_spacing(&req, GTK_CONTAINER(p->pwid), "");
    net_active_detect(tb);

    /* initialise defaults */
    tb->topxwin           = p->panel->topxwin;
    tb->tooltips          = 1;
    tb->icons_only        = 0;
    tb->accept_skip_pager = 1;
    tb->show_iconified    = 1;
    tb->show_mapped       = 1;
    tb->show_all_desks    = 0;
    tb->task_width_max    = TASK_WIDTH_MAX;
    tb->task_height_max   = p->panel->max_elem_height;
    tb->task_list         = g_hash_table_new(g_int_hash, g_int_equal);
    tb->focused_state     = GTK_STATE_ACTIVE;
    tb->normal_state      = GTK_STATE_NORMAL;
    tb->spacing           = 0;
    tb->use_mouse_wheel   = 1;
    tb->use_urgency_hint  = 1;

    /* read config overrides */
    XCG(xc, "tooltips",        &tb->tooltips,          enum, bool_enum);
    XCG(xc, "iconsonly",       &tb->icons_only,         enum, bool_enum);
    XCG(xc, "acceptskippager", &tb->accept_skip_pager,  enum, bool_enum);
    XCG(xc, "showiconified",   &tb->show_iconified,     enum, bool_enum);
    XCG(xc, "showalldesks",    &tb->show_all_desks,     enum, bool_enum);
    XCG(xc, "showmapped",      &tb->show_mapped,        enum, bool_enum);
    XCG(xc, "usemousewheel",   &tb->use_mouse_wheel,    enum, bool_enum);
    XCG(xc, "useurgencyhint",  &tb->use_urgency_hint,   enum, bool_enum);
    XCG(xc, "maxtaskwidth",    &tb->task_width_max,     int);

    /* FIXME: cap at TASK_HEIGHT_MAX until per-plugin height limit is ready */
    if (tb->task_height_max > TASK_HEIGHT_MAX)
        tb->task_height_max = TASK_HEIGHT_MAX;
    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        tb->iconsize = MIN(p->panel->ah, tb->task_height_max) - req.height;
        if (tb->icons_only)
            tb->task_width_max = tb->iconsize + req.width;
    } else {
        /* narrow vertical panels go icons-only automatically */
        if (p->panel->aw <= 30)
            tb->icons_only = 1;
        tb->iconsize = MIN(p->panel->aw, tb->task_height_max) - req.height;
        if (tb->icons_only)
            tb->task_width_max = tb->iconsize + req.height;
    }
    taskbar_build_gui(p);
    tb_net_client_list(NULL, tb);   /* populate initial window list */
    tb_display(tb);
    tb_net_active_window(NULL, tb); /* set initial focus state */
    RET(1);
}


/*
 * taskbar_destructor -- clean up all taskbar plugin resources.
 *
 * Removes the GDK event filter, disconnects all 6 FbEv signals,
 * removes all tasks, destroys the hash table, XFree's the window
 * list, and destroys the menu.
 */
static void
taskbar_destructor(plugin_instance *p)
{
    taskbar_priv *tb = (taskbar_priv *) p;

    ENTER;
    gdk_window_remove_filter(NULL, (GdkFilterFunc)tb_event_filter, tb);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            tb_net_current_desktop, tb);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            tb_net_active_window, tb);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            tb_net_number_of_desktops, tb);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            tb_net_client_list, tb);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            tb_make_menu, tb);

    g_hash_table_foreach_remove(tb->task_list, (GHRFunc) task_remove_every,
            NULL);
    g_hash_table_destroy(tb->task_list);
    if (tb->wins)
        XFree(tb->wins);
    /* tb->bar is a child of p->pwid — destroyed by framework; no explicit destroy needed */
    gtk_widget_destroy(tb->menu);
    DBG("alloc_no=%d\n", tb->alloc_no);
    RET();
}

static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "taskbar",
    .name        = "Taskbar",
    .version     = "1.0",
    .description = "Shows opened windows",
    .priv_size   = sizeof(taskbar_priv),

    .constructor = taskbar_constructor,
    .destructor  = taskbar_destructor,
};

/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
