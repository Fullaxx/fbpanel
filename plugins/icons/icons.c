/* icons.c -- Invisible icon-override plugin for fbpanel.
 *
 * This plugin runs invisibly (no visible widget in the panel bar) and
 * intercepts window-icon assignments. It can:
 *   - Assign user-configured per-application icons (matched by WM_CLASS)
 *     to windows that don't have a _NET_WM_ICON property.
 *   - Assign a configurable default icon to all windows with no icon.
 *
 * Mechanism:
 *   - Subscribes to the fbev "client_list" signal to be notified when the
 *     window list changes (do_net_client_list).
 *   - Registers a GDK root-window event filter (ics_event_filter) to catch
 *     PropertyNotify events on individual windows (WM_CLASS, WM_HINTS changes).
 *   - Icon data is converted from GdkPixbuf RGBA to the X11 ARGB format
 *     expected by _NET_WM_ICON and written via XChangeProperty.
 *
 * Memory:
 *   ics->wmpix -- linked list of wmpix_t; each holds a ARGB data array.
 *                 Freed by drop_config().
 *   ics->dicon -- single wmpix_t for the default icon; freed by drop_config().
 *   ics->task_list -- GHashTable<Window, task*>; tasks freed by free_task().
 *   ics->wins  -- XFree'd array of Window IDs.
 *
 * X11 resource management:
 *   XClassHint strings (res_name, res_class inside task) are allocated by
 *   Xlib and must be freed with XFree, not g_free.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
//#include <X11/xpm.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf-xlib/gdk-pixbuf-xlib.h>
#include <gdk/gdk.h>



#include "panel.h"
#include "misc.h"
#include "plugin.h"


//#define DEBUGPRN
#include "dbg.h"

/*
 * wmpix_t -- node in the user-configured per-application icon list.
 *
 * Memory:
 *   data         -- g_malloc'd ARGB pixel array; freed with g_free.
 *   ch.res_name  -- g_strdup'd; freed with g_free (not XFree -- see drop_config).
 *   ch.res_class -- g_strdup'd; freed with g_free.
 */
typedef struct wmpix_t {
    struct wmpix_t *next; /* intrusive singly-linked list pointer */
    gulong *data;         /* _NET_WM_ICON format: [width, height, ARGB...] */
    int size;             /* total number of gulong elements in data       */
    XClassHint ch;        /* WM_CLASS match criterion (NULL = wildcard)    */
} wmpix_t;

struct _icons;

/*
 * task -- tracked managed window.
 *
 * Memory:
 *   ch.res_name, ch.res_class -- allocated by XGetClassHint; freed with XFree.
 *                                Do NOT use g_free on these.
 */
typedef struct _task{
    struct _icons *ics; /* back-pointer to owning icons_priv instance */
    Window win;         /* X11 window ID                              */
    int refcount;       /* reference count for stale-removal logic    */
    XClassHint ch;      /* WM_CLASS strings from Xlib                */
} task;

/*
 * icons_priv -- per-instance state.
 *
 * Memory:
 *   wins      -- array of Window IDs from XGetWindowProperty; XFree'd.
 *   task_list -- GHashTable keyed by Window; values are task*.
 *   wmpix     -- head of per-app icon linked list; freed by drop_config.
 *   dicon     -- default icon node; freed by drop_config.
 */
typedef struct _icons{
    plugin_instance plugin; /* base class -- must be first */
    Window *wins;           /* _NET_CLIENT_LIST snapshot; XFree'd on update */
    int win_num;            /* number of entries in wins                     */
    GHashTable  *task_list; /* Window -> task* hash table                    */
    int num_tasks;          /* current count of tracked tasks                */
    wmpix_t *wmpix;         /* linked list of user-defined per-app icons     */
    wmpix_t *dicon;         /* default icon applied when no other icon found */
} icons_priv;

/* Forward declarations */
static void ics_propertynotify(icons_priv *ics, XEvent *ev);
static GdkFilterReturn ics_event_filter( XEvent *, GdkEvent *, icons_priv *);
static void icons_destructor(plugin_instance *p);

/******************************************/
/* Resource Release Code                  */
/******************************************/

/*
 * free_task -- release one task and optionally remove it from the hash table.
 *
 * Parameters:
 *   ics  -- owning icons_priv instance; num_tasks is decremented.
 *   tk   -- task to free; ch strings are XFree'd, tk itself is g_free'd.
 *   hdel -- non-zero: also remove tk from ics->task_list hash table.
 *
 * Memory: ch.res_class and ch.res_name were set by XGetClassHint so must be
 *         released with XFree, not g_free.
 */
static void
free_task(icons_priv *ics, task *tk, int hdel)
{
    ENTER;
    ics->num_tasks--;
    if (hdel)
        g_hash_table_remove(ics->task_list, &tk->win); // remove keyed by Window
    // XClassHint strings are Xlib-allocated; must use XFree
    if (tk->ch.res_class)
        XFree(tk->ch.res_class);
    if (tk->ch.res_name)
        XFree(tk->ch.res_name);
    g_free(tk); // the task struct itself is GLib-allocated
    RET();
}

/*
 * task_remove_every -- GHRFunc callback to free all tasks during full teardown.
 *
 * Parameters:
 *   win -- key (Window), unused here.
 *   tk  -- task value to free.
 *
 * Returns: TRUE (tells g_hash_table_foreach_remove to remove this entry).
 *
 * hdel=0: the hash table is being cleared by foreach_remove so we do not
 * need to call g_hash_table_remove from within the callback.
 */
static gboolean
task_remove_every(Window *win, task *tk)
{
    free_task(tk->ics, tk, 0); // 0 = don't remove from hash (iterator handles it)
    return TRUE; // instruct foreach_remove to remove this entry
}


/*
 * drop_config -- free all dynamically allocated icon data and task list.
 *
 * Parameters:
 *   ics -- icons_priv instance to reset.
 *
 * Frees:
 *   - All wmpix_t nodes in ics->wmpix (linked list).
 *   - The default icon ics->dicon.
 *   - All task entries in ics->task_list.
 *   - The window array ics->wins.
 *
 * After this call, ics is in the same state as freshly constructed
 * (except that ics->task_list still exists as an empty hash table).
 */
static void
drop_config(icons_priv *ics)
{
    wmpix_t *wp;

    ENTER;
    /* free application icons -- walk the singly-linked list */
    while (ics->wmpix)
    {
        wp = ics->wmpix;
        ics->wmpix = ics->wmpix->next; // advance before freeing
        g_free(wp->ch.res_name);   // g_strdup'd in read_application
        g_free(wp->ch.res_class);
        g_free(wp->data);           // ARGB pixel array
        g_free(wp);
    }

    /* free default icon */
    if (ics->dicon)
    {
        g_free(ics->dicon->data);
        g_free(ics->dicon);
        ics->dicon = NULL;
    }

    /* free task list -- removes and frees every entry */
    g_hash_table_foreach_remove(ics->task_list,
        (GHRFunc) task_remove_every, (gpointer)ics);

    if (ics->wins)
    {
        DBG("free ics->wins\n");
        XFree(ics->wins); // XProperty data is XFree'd, not g_free'd
        ics->wins = NULL;
    }
    RET();
}

/*
 * get_wmclass -- (re)read the WM_CLASS property of a window into tk->ch.
 *
 * Parameters:
 *   tk -- task whose window to query; tk->ch is updated in place.
 *
 * Existing ch strings are XFree'd before the new query so this is safe
 * to call repeatedly. If XGetClassHint fails, both fields are set to NULL.
 *
 * X11 error: XGetClassHint may generate a BadWindow error if the window
 * has been destroyed between the time it was added to the list and now.
 * No error handler is installed here -- a Badwindow X error will be printed
 * to stderr but will not crash the process (X sends async errors).
 */
static void
get_wmclass(task *tk)
{
    ENTER;
    // Free old Xlib-allocated strings before re-querying
    if (tk->ch.res_name)
        XFree(tk->ch.res_name);
    if (tk->ch.res_class)
        XFree(tk->ch.res_class);
    // XGetClassHint allocates res_name and res_class with Xlib internals
    if (!XGetClassHint (gdk_display, tk->win, &tk->ch))
        tk->ch.res_class = tk->ch.res_name = NULL; // window has no WM_CLASS
    DBG("name=%s class=%s\n", tk->ch.res_name, tk->ch.res_class);
    RET();
}




/*
 * find_task -- look up a task by window ID in the hash table.
 *
 * Parameters:
 *   ics -- icons_priv instance.
 *   win -- X11 window ID to look up.
 *
 * Returns: task* if found, NULL otherwise.
 *
 * The hash table is keyed by Window (gulong); g_int_hash/g_int_equal treat
 * the key as a pointer to int, so the key must be the address of the Window
 * field inside the task struct (stable for the task's lifetime).
 */
static inline task *
find_task (icons_priv * ics, Window win)
{
    ENTER;
    RET(g_hash_table_lookup(ics->task_list, &win));
}


/*
 * task_has_icon -- check whether a window already has any icon set.
 *
 * Parameters:
 *   tk -- task to check.
 *
 * Returns: 1 if the window has _NET_WM_ICON data OR WMHints icon pixmap/mask,
 *          0 otherwise.
 *
 * Used to decide whether to assign the default icon: we only assign if the
 * window has no icon of its own.
 */
static int task_has_icon(task *tk)
{
    XWMHints *hints;
    gulong *data;
    int n;

    ENTER;
    // Check for _NET_WM_ICON (modern EWMH icon)
    data = get_xaproperty(tk->win, a_NET_WM_ICON, XA_CARDINAL, &n);
    if (data)
    {
        XFree(data); // we only need to know it exists; don't use the data
        RET(1);
    }

    // Fallback: check for legacy WM icon pixmap hint
    hints = XGetWMHints(GDK_DISPLAY(), tk->win);
    if (hints)
    {
        if ((hints->flags & IconPixmapHint) || (hints->flags & IconMaskHint))
        {
            XFree (hints);
            RET(1); // window has a traditional icon
        }
        XFree (hints);
    }
    RET(0); // no icon found
}

/*
 * get_user_icon -- find the first wmpix_t from the config list that matches
 *                  the window's WM_CLASS.
 *
 * Parameters:
 *   ics -- icons_priv with the wmpix linked list.
 *   tk  -- task whose ch.res_class and ch.res_name are compared.
 *
 * Returns: wmpix_t* if a match is found, NULL otherwise.
 *
 * Matching logic:
 *   mc = true if wmpix entry has no class constraint OR classes match.
 *   mn = true if wmpix entry has no name  constraint OR names  match.
 *   A match requires both mc and mn to be true.
 */
static wmpix_t *
get_user_icon(icons_priv *ics, task *tk)
{
    wmpix_t *tmp;
    int mc, mn;

    ENTER;
    // If the window has no WM_CLASS at all, we cannot match any config entry
    if (!(tk->ch.res_class || tk->ch.res_name))
        RET(NULL);
    DBG("\nch.res_class=[%s] ch.res_name=[%s]\n", tk->ch.res_class,
        tk->ch.res_name);

    for (tmp = ics->wmpix; tmp; tmp = tmp->next)
    {
        DBG("tmp.res_class=[%s] tmp.res_name=[%s]\n", tmp->ch.res_class,
            tmp->ch.res_name);
        // NULL constraint == wildcard; non-NULL must match
        mc = !tmp->ch.res_class || !strcmp(tmp->ch.res_class, tk->ch.res_class);
        mn = !tmp->ch.res_name  || !strcmp(tmp->ch.res_name, tk->ch.res_name);
        DBG("mc=%d mn=%d\n", mc, mn);
        if (mc && mn)
        {
            DBG("match !!!!\n");
            RET(tmp); // first match wins
        }
    }
    RET(NULL); // no match found
}



/*
 * pixbuf2argb -- convert a GdkPixbuf to the _NET_WM_ICON ARGB gulong array.
 *
 * Parameters:
 *   pixbuf -- source GdkPixbuf (any depth; alpha handled if n_channels >= 4).
 *   size   -- output: number of gulong elements written to the returned array.
 *
 * Returns: g_malloc'd array of gulong in the format:
 *   [width, height, pixel0_ARGB, pixel1_ARGB, ... pixelN_ARGB]
 *   Caller must g_free() the returned pointer.
 *
 * The format stored in each gulong is 0xAARRGGBB packed into native byte order.
 * This matches the _NET_WM_ICON ICCCM/EWMH specification which expects 32-bit
 * ARGB values with the most significant byte = alpha.
 *
 * NOTE: on 64-bit systems, gulong is 8 bytes wide, so this array wastes 4 bytes
 * per pixel compared to using guint32. The X11 protocol packs them as 32-bit
 * values regardless (XChangeProperty with format=32 extracts the low 32 bits).
 */
gulong *
pixbuf2argb (GdkPixbuf *pixbuf, int *size)
{
    gulong *data;
    guchar *pixels;
    gulong *p;
    gint width, height, stride;
    gint x, y;
    gint n_channels;

    ENTER;
    *size = 0;
    width = gdk_pixbuf_get_width (pixbuf);
    height = gdk_pixbuf_get_height (pixbuf);
    stride = gdk_pixbuf_get_rowstride (pixbuf);    // bytes per row (may be > width*n_channels)
    n_channels = gdk_pixbuf_get_n_channels (pixbuf); // 3 (RGB) or 4 (RGBA)

    // Header (width + height) + one gulong per pixel
    *size += 2 + width * height;
    p = data = g_malloc (*size * sizeof (gulong)); // caller must g_free
    *p++ = width;   // ARGB array header: width first
    *p++ = height;  // then height

    pixels = gdk_pixbuf_get_pixels (pixbuf); // raw pixel byte array

    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
        {
            guchar r, g, b, a;

            // Extract RGBA bytes; stride accounts for row padding
            r = pixels[y*stride + x*n_channels + 0];
            g = pixels[y*stride + x*n_channels + 1];
            b = pixels[y*stride + x*n_channels + 2];
            if (n_channels >= 4)
                a = pixels[y*stride + x*n_channels + 3]; // alpha channel
            else
                a = 255; // assume fully opaque for RGB pixbufs

            // Pack into _NET_WM_ICON ARGB format: 0xAARRGGBB
            *p++ = a << 24 | r << 16 | g << 8 | b ;
        }
    }
    RET(data);
}



/*
 * set_icon_maybe -- assign an icon to a window if it needs one.
 *
 * Parameters:
 *   ics -- icons_priv with user icon list and default icon.
 *   tk  -- task whose window should receive the icon.
 *
 * Decision logic:
 *   1. Try get_user_icon (user-configured app-specific icon).
 *   2. If no match AND the window has its own icon: do nothing (return).
 *   3. If no match AND no own icon: try the default icon (ics->dicon).
 *   4. If a pix was found: write it to _NET_WM_ICON via XChangeProperty.
 *
 * After XChangeProperty, the window manager should re-read _NET_WM_ICON
 * and update the taskbar / switcher display.
 *
 * No X error handling: XChangeProperty may fail silently if the window is gone.
 */
static void
set_icon_maybe (icons_priv *ics, task *tk)
{
    wmpix_t *pix;

    ENTER;
    g_assert ((ics != NULL) && (tk != NULL));
    g_return_if_fail(tk != NULL); // redundant after assert but kept as defensive check

    pix = get_user_icon(ics, tk); // 1. look for user-configured icon
    if (!pix)
    {
        if (task_has_icon(tk))
            RET(); // 2. window already has an icon; don't override
        pix = ics->dicon; // 3. fall back to default icon
    }
    if (!pix)
        RET(); // neither user icon nor default icon configured

    DBG("%s size=%d\n", pix->ch.res_name, pix->size);
    // Write the ARGB data as _NET_WM_ICON property on the window
    XChangeProperty (GDK_DISPLAY(), tk->win,
          a_NET_WM_ICON, XA_CARDINAL, 32, PropModeReplace, (guchar*) pix->data, pix->size);

    RET();
}



/*
 * task_remove_stale -- GHRFunc callback: decrement refcount and remove if zero.
 *
 * Parameters:
 *   win -- key (Window), unused.
 *   tk  -- task to check.
 *
 * Returns: TRUE if the task's refcount reached zero (triggers removal + free).
 *
 * Called after refreshing ics->wins: tasks not seen in the new window list
 * have their refcount post-decremented; those that reach zero are removed.
 *
 * BUG: the refcount is decremented BEFORE the zero check (tk->refcount-- == 0
 *      is evaluated as (tk->refcount_old == 0), meaning a task with refcount=1
 *      will only be removed on the SECOND sweep with refcount=0, not the first.
 *      The initial refcount is set to 1 on insertion so the first sweep after
 *      removal reduces it to 0 (from 1), making the comparison (1 == 0) FALSE
 *      -- the task survives one extra cycle. Actually this is intentional: the
 *      new list sets refcount++ on existing tasks before the stale sweep, so
 *      a task present in the new list ends up with refcount>=1 and survives.
 *      A task gone from the list is never incremented, post-decrements from 1
 *      to 0, returns TRUE on the NEXT call (refcount=0 post-dec -> -1 == 0? No).
 *      This refcount scheme is subtle and may have an off-by-one edge case.
 */
/* tell to remove element with zero refcount */
static gboolean
task_remove_stale(Window *win, task *tk)
{
    ENTER;
    // Post-decrement: if refcount WAS 0 before decrement, remove now
    if (tk->refcount-- == 0)
    {
        free_task(tk->ics, tk, 0); // 0 = iterator will remove from hash
        RET(TRUE);
    }
    RET(FALSE); // refcount still positive; keep the task
}

/*****************************************************
 * handlers for NET actions                          *
 *****************************************************/

/*
 * ics_event_filter -- GDK root-window event filter for X11 PropertyNotify events.
 *
 * Parameters:
 *   xev   -- raw X11 event
 *   event -- GDK event (unused)
 *   ics   -- icons_priv instance (user data)
 *
 * Returns: GDK_FILTER_CONTINUE (let GDK process the event normally too).
 *
 * Only PropertyNotify events are dispatched to ics_propertynotify().
 * Registered with gdk_window_add_filter(NULL, ...) in icons_constructor.
 * Deregistered with gdk_window_remove_filter in icons_destructor.
 */
static GdkFilterReturn
ics_event_filter( XEvent *xev, GdkEvent *event, icons_priv *ics)
{

    ENTER;
    g_assert(ics != NULL);
    if (xev->type == PropertyNotify)
	ics_propertynotify(ics, xev); // dispatch to property handler
    RET(GDK_FILTER_CONTINUE); // always let GDK continue processing
}


/*
 * do_net_client_list -- refresh the tracked window list from _NET_CLIENT_LIST.
 *
 * Parameters:
 *   ics -- icons_priv instance.
 *
 * Called when fbev emits the "client_list" signal (window list changed).
 *
 * Algorithm:
 *   1. Free old wins array.
 *   2. Fetch new _NET_CLIENT_LIST from root window.
 *   3. For each window in the new list:
 *      a. If already tracked: increment refcount (marks it as still active).
 *      b. If new: create a task, get its WM_CLASS, assign an icon if needed,
 *         subscribe to PropertyChangeMask|StructureNotifyMask on the window,
 *         insert into hash table.
 *   4. Sweep the hash table: remove tasks whose refcount is now 0 (gone windows).
 *
 * Note: FBPANEL_WIN(tk->win) skips fbpanel's own windows from event selection.
 */
static void
do_net_client_list(icons_priv *ics)
{
    int i;
    task *tk;

    ENTER;
    if (ics->wins)
    {
        DBG("free ics->wins\n");
        XFree(ics->wins); // release previous window list (Xlib-allocated)
        ics->wins = NULL;
    }
    // Fetch the current managed window list from the root window
    ics->wins = get_xaproperty (GDK_ROOT_WINDOW(),
        a_NET_CLIENT_LIST, XA_WINDOW, &ics->win_num);
    if (!ics->wins)
	RET(); // property not set (no EWMH-compliant WM running?)
    DBG("alloc ics->wins\n");
    for (i = 0; i < ics->win_num; i++)
    {
        if ((tk = g_hash_table_lookup(ics->task_list, &ics->wins[i])))
        {
            // Existing window: bump refcount so it survives the stale sweep
            tk->refcount++;
        }
        else
        {
            // New window: allocate and initialise a task entry
            tk = g_new0(task, 1);
            tk->refcount++;      // refcount starts at 1
            ics->num_tasks++;
            tk->win = ics->wins[i];
            tk->ics = ics;       // back-pointer for callbacks

            // Select events on the window so we hear about WM_CLASS changes
            if (!FBPANEL_WIN(tk->win))
            {
                XSelectInput(GDK_DISPLAY(), tk->win,
                    PropertyChangeMask | StructureNotifyMask);
            }
            get_wmclass(tk);         // fetch WM_CLASS strings
            set_icon_maybe(ics, tk); // assign icon if needed
            g_hash_table_insert(ics->task_list, &tk->win, tk);
        }
    }

    /* remove windows that aren't in the NET_CLIENT_LIST anymore */
    g_hash_table_foreach_remove(ics->task_list,
        (GHRFunc) task_remove_stale, NULL);
    RET();
}

/*
 * ics_propertynotify -- handle X11 PropertyNotify events on tracked windows.
 *
 * Parameters:
 *   ics -- icons_priv instance.
 *   ev  -- PropertyNotify XEvent.
 *
 * Handles:
 *   XA_WM_CLASS -- application class changed; re-fetch and possibly re-assign icon.
 *   XA_WM_HINTS -- window hints changed; re-check if icon needs assigning.
 *
 * Events on the root window are ignored (those are handled by fbev signals).
 * Events for untracked windows are ignored.
 */
static void
ics_propertynotify(icons_priv *ics, XEvent *ev)
{
    Atom at;
    Window win;


    ENTER;
    win = ev->xproperty.window;
    at = ev->xproperty.atom;
    DBG("win=%lx at=%ld\n", win, at);
    if (win != GDK_ROOT_WINDOW()) // root window events handled elsewhere
    {
	task *tk = find_task(ics, win);

        if (!tk)
            RET(); // window not in our task list; ignore
        if (at == XA_WM_CLASS)
        {
            // Application class changed: re-match user icon config
            get_wmclass(tk);
            set_icon_maybe(ics, tk);
        }
        else if (at == XA_WM_HINTS)
        {
            // WM hints changed: check if the window now has or lost an icon
            set_icon_maybe(ics, tk);
        }
    }
    RET();
}


/*
 * read_application -- parse one <application> xconf block and add to wmpix list.
 *
 * Parameters:
 *   ics -- icons_priv to update (new wmpix_t prepended to ics->wmpix).
 *   xc  -- xconf block with "image", "icon", "appname", "classname" keys.
 *
 * Returns: 1 on success, 0 on error (missing image/icon key).
 *
 * Memory: wmpix_t and its data are g_malloc'd; ch strings are g_strdup'd.
 *         expand_tilda(fname) allocates; freed with g_free.
 *         gp (GdkPixbuf) is unref'd after conversion.
 *
 * Note: if appname or classname is NULL it is stored as NULL (acts as a
 *       wildcard in get_user_icon).
 */
static int
read_application(icons_priv *ics, xconf *xc)
{
    GdkPixbuf *gp = NULL;

    gchar *fname, *iname, *appname, *classname;
    wmpix_t *wp = NULL;
    gulong *data;
    int size;

    ENTER;
    iname = fname = appname = classname = NULL;
    XCG(xc, "image", &fname, str);       // path to image file (optional)
    XCG(xc, "icon", &iname, str);        // theme icon name (optional)
    XCG(xc, "appname", &appname, str);   // WM_CLASS res_name to match (NULL = any)
    XCG(xc, "classname", &classname, str); // WM_CLASS res_class to match (NULL = any)
    fname = expand_tilda(fname);          // expand ~ to home directory

    DBG("appname=%s classname=%s\n", appname, classname);
    if (!(fname || iname))
        goto error; // must specify at least one of image/icon
    // Load the pixbuf from icon name or file path, scaled to 48x48
    gp = fb_pixbuf_new(iname, fname, 48, 48, FALSE);
    if (gp)
    {
        if ((data = pixbuf2argb(gp, &size))) // convert to ARGB gulong array
        {
            wp = g_new0 (wmpix_t, 1);
            wp->next = ics->wmpix;  // prepend to the list (LIFO order)
            wp->data = data;
            wp->size = size;
            // NULL appname/classname means wildcard; g_strdup(NULL) returns NULL
            wp->ch.res_name = g_strdup(appname);
            wp->ch.res_class = g_strdup(classname);
            DBG("read name=[%s] class=[%s]\n",
                wp->ch.res_name, wp->ch.res_class);
            ics->wmpix = wp; // new head of list
        }
        g_object_unref(gp); // release the pixbuf; data array is our copy
    }
    g_free(fname); // expand_tilda allocates; must free
    RET(1);

error:
    g_free(fname);
    RET(0);
}

/*
 * read_dicon -- load and convert the default icon from a file.
 *
 * Parameters:
 *   ics  -- icons_priv to update; ics->dicon is set on success.
 *   name -- path string (may contain ~); expand_tilda is called.
 *
 * Returns: 1 on success, 0 if name was NULL or file could not be loaded.
 *
 * Memory: ics->dicon is g_malloc'd; must be freed in drop_config.
 *         expand_tilda(name) allocates; freed here.
 */
static int
read_dicon(icons_priv *ics, gchar *name)
{
    gchar *fname;
    GdkPixbuf *gp;
    int size;
    gulong *data;

    ENTER;
    fname = expand_tilda(name);
    if (!fname)
        RET(0); // name was NULL
    gp = gdk_pixbuf_new_from_file(fname, NULL); // load the image; errors ignored
    if (gp)
    {
        if ((data = pixbuf2argb(gp, &size)))
        {
            ics->dicon = g_new0 (wmpix_t, 1); // single default icon node
            ics->dicon->data = data;
            ics->dicon->size = size;
            // ch fields left at NULL/zero: not used for matching
        }
        g_object_unref(gp);
    }
    g_free(fname); // expand_tilda allocates
    RET(1);
}


/*
 * ics_parse_config -- read the plugin's xconf and build the icon lists.
 *
 * Parameters:
 *   ics -- icons_priv instance; reads xconf via (plugin_instance*)ics->xc.
 *
 * Returns: 1 on success, 0 on error.
 *
 * Reads:
 *   defaulticon -- path to the default icon image.
 *   application -- zero or more application-specific icon blocks.
 */
static int
ics_parse_config(icons_priv *ics)
{
    gchar *def_icon;
    plugin_instance *p = (plugin_instance *) ics;
    int i;
    xconf *pxc;

    ENTER;
    def_icon = NULL;
    XCG(p->xc, "defaulticon", &def_icon, str); // optional default icon path
    if (def_icon && !read_dicon(ics, def_icon))
        goto error;

    // Iterate all <application> child blocks
    for (i = 0; (pxc = xconf_find(p->xc, "application", i)); i++)
        if (!read_application(ics, pxc))
            goto error;
    RET(1);

error:
    RET(0);
}

/*
 * theme_changed -- callback for icon theme "changed" signal.
 *
 * Parameters:
 *   ics -- icons_priv instance (swapped connection: ics is first arg).
 *
 * Rebuilds the entire configuration from scratch when the GTK icon theme
 * changes (e.g. user switches themes). This ensures theme icon names are
 * re-resolved against the new theme.
 *
 * Signal connection: gtk_icon_theme_get_default() "changed" (swapped).
 * Disconnected in icons_destructor.
 */
static void
theme_changed(icons_priv *ics)
{
    ENTER;
    drop_config(ics);      // free existing icon data
    ics_parse_config(ics); // re-read config and load icons from new theme
    do_net_client_list(ics); // re-apply icons to all current windows
    RET();
}

/*
 * icons_constructor -- set up the invisible icon-override plugin.
 *
 * Parameters:
 *   p -- plugin_instance.
 *
 * Returns: 1 on success.
 *
 * Does NOT add any visible widget to p->pwid.
 *
 * Sets up:
 *   - ics->task_list (GHashTable<Window, task*>).
 *   - Calls theme_changed() to parse config and apply to existing windows.
 *   - Connects "changed" on the global GTK icon theme.
 *   - Connects "client_list" on fbev.
 *   - Registers a GDK root-window event filter for PropertyNotify.
 */
static int
icons_constructor(plugin_instance *p)
{
    icons_priv *ics;

    ENTER;
    ics = (icons_priv *) p;
    // Create the window-to-task hash table (keyed by Window = gulong)
    ics->task_list = g_hash_table_new(g_int_hash, g_int_equal);
    theme_changed(ics); // load config and populate existing windows
    // Re-parse config and re-apply icons whenever the icon theme changes
    g_signal_connect_swapped(G_OBJECT(gtk_icon_theme_get_default()),
        "changed", (GCallback) theme_changed, ics);
    // Refresh window list when EWMH client list changes
    g_signal_connect_swapped(G_OBJECT (fbev), "client_list",
        G_CALLBACK (do_net_client_list), (gpointer) ics);
    // Install root-window filter for per-window PropertyNotify events
    gdk_window_add_filter(NULL, (GdkFilterFunc)ics_event_filter, ics );

    RET(1);
}


/*
 * icons_destructor -- release all resources.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 *
 * Disconnects all signals, removes the GDK filter, frees all icon and task
 * data, and destroys the hash table.
 */
static void
icons_destructor(plugin_instance *p)
{
    icons_priv *ics = (icons_priv *) p;

    ENTER;
    // Disconnect fbev signal to stop receiving window-list updates
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev), do_net_client_list,
        ics);
    // Disconnect icon-theme signal
    g_signal_handlers_disconnect_by_func(G_OBJECT(gtk_icon_theme_get_default()),
        theme_changed, ics);
    // Remove the GDK event filter for PropertyNotify
    gdk_window_remove_filter(NULL, (GdkFilterFunc)ics_event_filter, ics );
    drop_config(ics);                       // free all icon and task data
    g_hash_table_destroy(ics->task_list);   // destroy the now-empty hash table
    RET();
}

/* Plugin class descriptor.
 * invisible=1 means no widget is added to the panel bar. */
static plugin_class class = {
    .count     = 0,
    .invisible = 1,   /* this plugin adds no visible element to the panel */

    .type        = "icons",
    .name        = "Icons",
    .version     = "1.0",
    .description = "Invisible plugin to change window icons",
    .priv_size   = sizeof(icons_priv),


    .constructor = icons_constructor,
    .destructor  = icons_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
