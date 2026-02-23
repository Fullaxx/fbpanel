/*
 * fb-background-monitor.c:
 *
 * Copyright (C) 2001, 2002 Ian McKellar <yakk@yakk.net>
 *                     2002 Sun Microsystems, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *      Ian McKellar <yakk@yakk.net>
 *      Mark McLoughlin <mark@skynet.ie>
 */

/*
 * ev.c - FbEv: EWMH desktop event signal bus for fbpanel.
 *
 * Overview
 * --------
 * FbEv is a GObject that acts as a central signal dispatcher for
 * Extended Window Manager Hints (EWMH) property-change events coming
 * from the root window.  When the panel's X event loop detects a
 * PropertyNotify on the root window, it calls fb_ev_trigger() with
 * the appropriate signal index (one of the EV_* enum values from ev.h).
 *
 * Each signal has a default class handler that invalidates the locally
 * cached copy of the relevant EWMH value (sets it to -1, None, or NULL).
 * Lazy re-fetch happens the next time a fb_ev_*() accessor is called.
 *
 * Signals defined (see ev.h enum):
 *   EV_CURRENT_DESKTOP       - _NET_CURRENT_DESKTOP changed
 *   EV_NUMBER_OF_DESKTOPS    - _NET_NUMBER_OF_DESKTOPS changed
 *   EV_DESKTOP_NAMES         - _NET_DESKTOP_NAMES changed
 *   EV_ACTIVE_WINDOW         - _NET_ACTIVE_WINDOW changed
 *   EV_CLIENT_LIST_STACKING  - _NET_CLIENT_LIST_STACKING changed
 *   EV_CLIENT_LIST           - _NET_CLIENT_LIST changed
 *
 * Caching strategy
 * ----------------
 * Each EWMH value is cached in the FbEv instance struct.  Sentinel
 * values (-1 for integers, None for Window, NULL for pointers) indicate
 * "not yet fetched" or "invalidated".  Accessors fetch from the X server
 * on demand and cache the result.  The default signal handlers reset the
 * sentinel, forcing a re-fetch on the next accessor call.
 *
 * Memory ownership
 * ----------------
 * - ev->desktop_names: allocated by GLib (via get_xaproperty / strdup
 *   chain); freed with g_strfreev() in ev_desktop_names handler.
 * - ev->client_list, ev->client_list_stacking: allocated by Xlib via
 *   XGetWindowProperty; freed with XFree() in their handlers.
 * - FbEv itself is a GObject; callers must g_object_unref() when done.
 *
 * NOTE: The file header says "fb-background-monitor.c" — this appears to
 * be a copy-paste error; this file implements FbEv, not FbBg.
 */

#include <glib.h>
#include <glib-object.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "ev.h"
#include "misc.h"

//#define DEBUGPRN
#include "dbg.h"



/*
 * _FbEvClass - the GObject class structure for FbEv.
 *
 * Fields:
 *   parent_class           - GObjectClass we inherit from.
 *   dummy                  - unused padding (alignment artifact or reserved slot).
 *   current_desktop        - default class handler for EV_CURRENT_DESKTOP signal.
 *   active_window          - default class handler for EV_ACTIVE_WINDOW signal.
 *   number_of_desktops     - default class handler for EV_NUMBER_OF_DESKTOPS signal.
 *   desktop_names          - default class handler for EV_DESKTOP_NAMES signal.
 *   client_list            - default class handler for EV_CLIENT_LIST signal.
 *   client_list_stacking   - default class handler for EV_CLIENT_LIST_STACKING signal.
 *
 * Each handler slot is wired in fb_ev_class_init() and implements cache
 * invalidation for the corresponding EWMH value.
 */
struct _FbEvClass {
    GObjectClass   parent_class;
    void *dummy;                                         // unused; reserved/padding
    void (*current_desktop)(FbEv *ev, gpointer p);
    void (*active_window)(FbEv *ev, gpointer p);
    void (*number_of_desktops)(FbEv *ev, gpointer p);
    void (*desktop_names)(FbEv *ev, gpointer p);
    void (*client_list)(FbEv *ev, gpointer p);
    void (*client_list_stacking)(FbEv *ev, gpointer p);
};

/*
 * _FbEv - the GObject instance structure for FbEv.
 *
 * Cached EWMH state fields (all lazily populated):
 *   current_desktop        - index of the active virtual desktop; -1 = not cached.
 *   number_of_desktops     - total number of virtual desktops; -1 = not cached.
 *   desktop_names          - NULL-terminated array of desktop name strings;
 *                            NULL = not cached.  Owned by this struct; freed
 *                            with g_strfreev().
 *   active_window          - X11 Window ID of the currently focused window;
 *                            None = not cached.
 *   client_list            - array of all managed window IDs (_NET_CLIENT_LIST);
 *                            NULL = not cached.  Owned by this struct; freed
 *                            with XFree().
 *   client_list_stacking   - array of all managed window IDs in stacking order
 *                            (_NET_CLIENT_LIST_STACKING); NULL = not cached.
 *                            Owned by this struct; freed with XFree().
 *
 * Leftover X11 fields (appear to be copied from FbBg struct but are unused):
 *   xroot   - root Window ID (not used in ev.c).
 *   id      - Atom (not used in ev.c).
 *   gc      - X11 GC (not used in ev.c; commented-out XFreeGC in finalize).
 *   dpy     - Display pointer (not used in ev.c).
 *   pixmap  - X11 Pixmap ID (not used in ev.c).
 *
 * NOTE: The xroot/id/gc/dpy/pixmap fields are dead weight — they are never
 * initialised or used in this file.  They appear to have been copied from
 * the FbBg struct and forgotten.  This wastes memory and is misleading.
 */
struct _FbEv {
    GObject    parent_instance;

    int current_desktop;           // cached _NET_CURRENT_DESKTOP; -1 means stale
    int number_of_desktops;        // cached _NET_NUMBER_OF_DESKTOPS; -1 means stale
    char **desktop_names;          // cached _NET_DESKTOP_NAMES; NULL means stale
    Window active_window;          // cached _NET_ACTIVE_WINDOW; None means stale
    Window *client_list;           // cached _NET_CLIENT_LIST; NULL means stale
    Window *client_list_stacking;  // cached _NET_CLIENT_LIST_STACKING; NULL means stale

    // Fields below are unused remnants, apparently copied from FbBg struct:
    Window   xroot;   // unused
    Atom     id;      // unused
    GC       gc;      // unused (XFreeGC call is commented out in finalize)
    Display *dpy;     // unused
    Pixmap   pixmap;  // unused
};

/* Forward declarations for static (module-private) functions. */
static void fb_ev_class_init (FbEvClass *klass);
static void fb_ev_init (FbEv *monitor);
static void fb_ev_finalize (GObject *object);

// Default class handlers for each EWMH signal — perform cache invalidation:
static void ev_current_desktop(FbEv *ev, gpointer p);
static void ev_active_window(FbEv *ev, gpointer p);
static void ev_number_of_desktops(FbEv *ev, gpointer p);
static void ev_desktop_names(FbEv *ev, gpointer p);
static void ev_client_list(FbEv *ev, gpointer p);
static void ev_client_list_stacking(FbEv *ev, gpointer p);

/*
 * signals[] - array of GObject signal IDs registered in fb_ev_class_init.
 * Indexed by the EV_* enum values defined in ev.h.
 * EV_LAST_SIGNAL entries are reserved; only EV_CURRENT_DESKTOP ..
 * EV_CLIENT_LIST are actually registered.
 */
static guint signals [EV_LAST_SIGNAL] = { 0 };


/*
 * fb_ev_get_type - GObject type registration for FbEv.
 *
 * Returns: the GType ID for FbEv, registering it on the first call.
 *
 * Standard GObject boilerplate.  Type is registered as a plain GObject
 * (G_TYPE_OBJECT) with no special flags.
 * Thread-safety: NOT thread-safe (no locking on the static variable).
 * Acceptable since fbpanel is single-threaded.
 */
GType
fb_ev_get_type (void)
{
    static GType object_type = 0;

    if (!object_type) {
        static const GTypeInfo object_info = {
            sizeof (FbEvClass),
            (GBaseInitFunc) NULL,
            (GBaseFinalizeFunc) NULL,
            (GClassInitFunc) fb_ev_class_init,
            NULL,           /* class_finalize */
            NULL,           /* class_data */
            sizeof (FbEv),
            0,              /* n_preallocs */
            (GInstanceInitFunc) fb_ev_init,
        };

        object_type = g_type_register_static (
            G_TYPE_OBJECT, "FbEv", &object_info, 0);
    }

    return object_type;
}



/*
 * fb_ev_class_init - GObject class initialiser for FbEvClass.
 *
 * Parameters:
 *   klass - the FbEvClass being initialised.
 *
 * Responsibilities:
 *   1. Registers all six EWMH signals via g_signal_new().
 *      Each signal:
 *        - uses G_SIGNAL_RUN_FIRST (class handler fires before user callbacks)
 *        - takes no arguments (VOID__VOID marshal)
 *        - returns void
 *      The G_STRUCT_OFFSET macro points each signal at the corresponding
 *      function pointer slot in FbEvClass.
 *   2. Wires default class handlers (ev_*) to the FbEvClass vtable slots.
 *      These handlers perform cache invalidation.
 *   3. Overrides GObjectClass::finalize with fb_ev_finalize.
 *
 * NOTE: Signals are registered in a different order than the EV_* enum
 * in ev.h.  EV_CLIENT_LIST_STACKING (index 4) is registered before
 * EV_CLIENT_LIST (index 5), matching the enum order.  The registration
 * order must match the enum so that signals[EV_X] stores the correct ID.
 */
static void
fb_ev_class_init (FbEvClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    // Register "current_desktop" signal (EV_CURRENT_DESKTOP = 0)
    signals [EV_CURRENT_DESKTOP] =
        g_signal_new ("current_desktop",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, current_desktop),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);

    // Register "number_of_desktops" signal (EV_NUMBER_OF_DESKTOPS = 1)
    signals [EV_NUMBER_OF_DESKTOPS] =
        g_signal_new ("number_of_desktops",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, number_of_desktops),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);

    // Register "desktop_names" signal (EV_DESKTOP_NAMES = 2)
    signals [EV_DESKTOP_NAMES] =
        g_signal_new ("desktop_names",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, desktop_names),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);

    // Register "active_window" signal (EV_ACTIVE_WINDOW = 3)
    signals [EV_ACTIVE_WINDOW] =
        g_signal_new ("active_window",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, active_window),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);

    // Register "client_list_stacking" signal (EV_CLIENT_LIST_STACKING = 4)
    signals [EV_CLIENT_LIST_STACKING] =
        g_signal_new ("client_list_stacking",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, client_list_stacking),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);

    // Register "client_list" signal (EV_CLIENT_LIST = 5)
    signals [EV_CLIENT_LIST] =
        g_signal_new ("client_list",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, client_list),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);

    object_class->finalize = fb_ev_finalize;  // override finalize for cleanup

    // Wire default class handler vtable slots to cache-invalidation functions
    klass->current_desktop      = ev_current_desktop;
    klass->active_window        = ev_active_window;
    klass->number_of_desktops   = ev_number_of_desktops;
    klass->desktop_names        = ev_desktop_names;
    klass->client_list          = ev_client_list;
    klass->client_list_stacking = ev_client_list_stacking;
}

/*
 * fb_ev_init - GObject instance initialiser for FbEv.
 *
 * Parameters:
 *   ev - the newly allocated FbEv instance (GObject zeroes all fields first).
 *
 * Sets all cached EWMH values to their "not yet fetched" sentinel values:
 *   - Integers initialised to -1 (sentinel for "unknown / stale").
 *   - Window ID initialised to None (0).
 *   - Pointer fields left NULL (already zero from GObject allocation).
 *
 * NOTE: The unused xroot/id/gc/dpy/pixmap fields remain zero-initialised
 * by GObject, which is safe since they are never used.
 */
static void
fb_ev_init (FbEv *ev)
{
    ev->number_of_desktops = -1;          // -1 = not yet fetched from X server
    ev->current_desktop    = -1;          // -1 = not yet fetched from X server
    ev->active_window      = None;        // None = not yet fetched from X server
    ev->client_list_stacking = NULL;      // NULL = not yet fetched (already 0, explicit for clarity)
    ev->client_list          = NULL;      // NULL = not yet fetched (already 0, explicit for clarity)
}


/*
 * fb_ev_new - allocate and return a new FbEv instance.
 *
 * Returns: a new FbEv with reference count 1.
 *          Caller must eventually call g_object_unref().
 *
 * Unlike FbBg, FbEv has no singleton wrapper here; the caller manages
 * lifetime directly (though panel.c likely creates one global instance).
 */
FbEv *
fb_ev_new()
{
    return  g_object_new (FB_TYPE_EV, NULL);
}

/*
 * fb_ev_finalize - GObject finalize override; cleans up FbEv resources.
 *
 * Parameters:
 *   object - the GObject being destroyed (cast to FbEv internally).
 *
 * Called by the GObject machinery when the reference count reaches zero.
 *
 * NOTE: The body is almost empty.  The XFreeGC call is commented out because
 * ev->gc is never initialised (the X11 fields are unused dead code).
 * However, the cached EWMH data (desktop_names, client_list,
 * client_list_stacking) is NOT freed here — it is only freed inside the
 * signal handlers (ev_desktop_names, ev_client_list, ev_client_list_stacking).
 * If those signals are never emitted before the object is destroyed, or if
 * the data was fetched after the last signal, these allocations will LEAK.
 *
 * Specifically:
 *   ev->desktop_names        — must be g_strfreev()'d; NOT done in finalize.
 *   ev->client_list          — must be XFree()'d; NOT done in finalize.
 *   ev->client_list_stacking — must be XFree()'d; NOT done in finalize.
 *
 * The parent class finalize is also not chained (same issue as FbBg).
 */
static void
fb_ev_finalize (GObject *object)
{
    FbEv *ev G_GNUC_UNUSED;  // G_GNUC_UNUSED suppresses unused-variable warning

    ev = FB_EV (object);
    //XFreeGC(ev->dpy, ev->gc);  // intentionally commented out; gc/dpy are never set
    // BUG: ev->desktop_names, ev->client_list, ev->client_list_stacking
    //      are NOT freed here, causing memory leaks if they hold live data.
}

/*
 * fb_ev_trigger - emit a specific EWMH signal on an FbEv instance.
 *
 * Parameters:
 *   ev     - the FbEv instance to emit the signal on.
 *   signal - the signal index (one of the EV_* enum values from ev.h).
 *            Must satisfy: 0 <= signal < EV_LAST_SIGNAL.
 *
 * This is the primary external API used by the panel's X event handler.
 * When a PropertyNotify arrives for an EWMH atom, the panel maps the atom
 * to an EV_* index and calls fb_ev_trigger().
 *
 * Emission sequence:
 *   1. The default class handler (ev_*) fires first (G_SIGNAL_RUN_FIRST),
 *      invalidating the cached value.
 *   2. All user-connected callbacks fire (plugins/widgets react to the change).
 *
 * Panics (g_assert) if signal is out of range.
 */
void
fb_ev_trigger(FbEv *ev, int signal)
{
    DBG("signal=%d\n", signal);
    g_assert(signal >=0 && signal < EV_LAST_SIGNAL);  // crash on invalid index in debug builds
    DBG("\n");
    g_signal_emit(ev, signals [signal], 0);  // emit signal with no extra arguments
}

/*
 * ev_current_desktop - default class handler for EV_CURRENT_DESKTOP signal.
 *
 * Parameters:
 *   ev - the FbEv instance.
 *   p  - unused; present to match the signal handler signature.
 *
 * Invalidates the cached current desktop index by resetting it to -1.
 * The next call to fb_ev_current_desktop() will re-fetch from the X server.
 */
static void
ev_current_desktop(FbEv *ev, gpointer p)
{
    ENTER;
    ev->current_desktop = -1;  // sentinel: force re-fetch on next accessor call
    RET();
}

/*
 * ev_active_window - default class handler for EV_ACTIVE_WINDOW signal.
 *
 * Parameters:
 *   ev - the FbEv instance.
 *   p  - unused; present to match the signal handler signature.
 *
 * Invalidates the cached active window by resetting it to None.
 * The next call to fb_ev_active_window() would re-fetch — but see the
 * note on fb_ev_active_window being declared but not implemented below.
 */
static void
ev_active_window(FbEv *ev, gpointer p)
{
    ENTER;
    ev->active_window = None;  // sentinel: force re-fetch on next accessor call
    RET();
}

/*
 * ev_number_of_desktops - default class handler for EV_NUMBER_OF_DESKTOPS signal.
 *
 * Parameters:
 *   ev - the FbEv instance.
 *   p  - unused; present to match the signal handler signature.
 *
 * Invalidates the cached desktop count by resetting it to -1.
 * The next call to fb_ev_number_of_desktops() will re-fetch from X server.
 */
static void
ev_number_of_desktops(FbEv *ev, gpointer p)
{
    ENTER;
    ev->number_of_desktops = -1;  // sentinel: force re-fetch on next accessor call
    RET();
}

/*
 * ev_desktop_names - default class handler for EV_DESKTOP_NAMES signal.
 *
 * Parameters:
 *   ev - the FbEv instance.
 *   p  - unused; present to match the signal handler signature.
 *
 * Frees the cached desktop names array (if any) and resets to NULL.
 * g_strfreev() correctly frees both the individual strings and the
 * pointer array itself (equivalent to a loop of g_free + g_free on array).
 *
 * Memory: ev->desktop_names is owned by FbEv; freed here with g_strfreev().
 */
static void
ev_desktop_names(FbEv *ev, gpointer p)
{
    ENTER;
    if (ev->desktop_names) {
        g_strfreev (ev->desktop_names);  // free all strings and the pointer array
        ev->desktop_names = NULL;        // reset to sentinel so accessor re-fetches
    }
    RET();
}

/*
 * ev_client_list - default class handler for EV_CLIENT_LIST signal.
 *
 * Parameters:
 *   ev - the FbEv instance.
 *   p  - unused; present to match the signal handler signature.
 *
 * Frees the cached _NET_CLIENT_LIST window array (if any) and resets to NULL.
 * The array was allocated by Xlib's XGetWindowProperty and must be freed
 * with XFree(), not g_free() or free().
 *
 * Memory: ev->client_list is owned by FbEv; freed here with XFree().
 */
static void
ev_client_list(FbEv *ev, gpointer p)
{
    ENTER;
    if (ev->client_list) {
        XFree(ev->client_list);  // use XFree because Xlib allocated this buffer
        ev->client_list = NULL;  // reset to sentinel so accessor re-fetches
    }
    RET();
}

/*
 * ev_client_list_stacking - default class handler for EV_CLIENT_LIST_STACKING.
 *
 * Parameters:
 *   ev - the FbEv instance.
 *   p  - unused; present to match the signal handler signature.
 *
 * Frees the cached _NET_CLIENT_LIST_STACKING window array and resets to NULL.
 * Same ownership rules as ev_client_list: buffer from Xlib, must use XFree().
 *
 * Memory: ev->client_list_stacking is owned by FbEv; freed here with XFree().
 */
static void
ev_client_list_stacking(FbEv *ev, gpointer p)
{
    ENTER;
    if (ev->client_list_stacking) {
        XFree(ev->client_list_stacking);  // use XFree because Xlib allocated this buffer
        ev->client_list_stacking = NULL;  // reset to sentinel so accessor re-fetches
    }
    RET();
}

/*
 * fb_ev_current_desktop - return the current virtual desktop index.
 *
 * Parameters:
 *   ev - a valid FbEv instance.
 *
 * Returns: the 0-based index of the currently active virtual desktop.
 *          Returns 0 as a fallback if the X property is not set.
 *
 * Lazy-fetch: if ev->current_desktop == -1 (cache invalid), reads the
 * _NET_CURRENT_DESKTOP cardinal property from the root window via
 * get_xaproperty() (defined in misc.c/misc.h).  The fetched value is
 * cached for subsequent calls.  On failure, defaults to 0.
 *
 * Memory: `data` from get_xaproperty is an Xlib allocation; freed with
 * XFree() immediately after copying the integer value out.
 *
 * NOTE: get_xaproperty uses GDK_ROOT_WINDOW() rather than ev->xroot.
 * This is consistent (both refer to the same default root), but it means
 * this code always uses the default screen regardless of which screen
 * the panel is on — a multi-screen limitation noted in ev.h.
 */
int
fb_ev_current_desktop(FbEv *ev)
{
    ENTER;
    if (ev->current_desktop == -1) {  // cache miss — need to fetch from X server
        guint *data;

        // Read _NET_CURRENT_DESKTOP cardinal from root window
        data = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, XA_CARDINAL, 0);
        if (data) {
            ev->current_desktop = *data;  // copy value out of Xlib buffer
            XFree (data);                 // release Xlib-allocated buffer
        } else
            ev->current_desktop = 0;      // property not set; default to desktop 0
    }
    RET(ev->current_desktop);
}

/*
 * fb_ev_number_of_desktops - return the total number of virtual desktops.
 *
 * Parameters:
 *   ev - a valid FbEv instance.
 *
 * Returns: the total number of virtual desktops, or 0 if not set by WM.
 *
 * Lazy-fetch: if ev->number_of_desktops == -1, reads _NET_NUMBER_OF_DESKTOPS
 * from the root window.  The fetched value is cached.  Defaults to 0 on
 * failure.
 *
 * Memory: same as fb_ev_current_desktop — XFree() the Xlib buffer.
 *
 * NOTE: Leading whitespace before `if` on line 286 is a minor formatting
 * inconsistency (extra space).
 */
int
fb_ev_number_of_desktops(FbEv *ev)
{
    ENTER;
     if (ev->number_of_desktops == -1) {  // cache miss — need to fetch from X server
        guint *data;

        // Read _NET_NUMBER_OF_DESKTOPS cardinal from root window
        data = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_NUMBER_OF_DESKTOPS, XA_CARDINAL, 0);
        if (data) {
            ev->number_of_desktops = *data;  // copy value out of Xlib buffer
            XFree (data);                    // release Xlib-allocated buffer
        } else
            ev->number_of_desktops = 0;      // property not set; default to 0
    }
    RET(ev->number_of_desktops);

}

/*
 * The following three function declarations appear at the bottom of the .c file.
 * These are DECLARATIONS only (no bodies), meaning these functions are
 * implemented elsewhere (presumably in another .c file, or they are stubs
 * that were never completed).
 *
 * This is highly unusual — function declarations belong in header files, not
 * in .c files.  Having declarations without definitions here means the linker
 * will look for these symbols in other translation units.  If they are not
 * found, the build will fail with undefined reference errors.
 *
 * fb_ev_active_window:       returns the _NET_ACTIVE_WINDOW Window ID.
 * fb_ev_client_list:         returns the _NET_CLIENT_LIST Window array.
 * fb_ev_client_list_stacking:returns the _NET_CLIENT_LIST_STACKING Window array.
 *
 * All three are declared in ev.h as well, which is the correct place.
 * These declarations here are redundant at best and confusing at worst.
 */
Window fb_ev_active_window(FbEv *ev);           // declared but NOT defined in this file
Window *fb_ev_client_list(FbEv *ev);            // declared but NOT defined in this file
Window *fb_ev_client_list_stacking(FbEv *ev);   // declared but NOT defined in this file
