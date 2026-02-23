/*
 * fb-background-monitor.h:
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
 *	Mark McLoughlin <mark@skynet.ie>
 */

/*
 * ev.h - Public interface for FbEv, the EWMH desktop event signal bus.
 *
 * FbEv is a GObject that dispatches Extended Window Manager Hints (EWMH)
 * property-change notifications as GObject signals.  The panel's X event
 * handler calls fb_ev_trigger() with an EV_* index when it detects a
 * PropertyNotify on the root window for a known EWMH atom.
 *
 * Plugins connect to the signals using g_signal_connect():
 *   g_signal_connect(fbev, "current_desktop", G_CALLBACK(my_cb), data);
 *
 * The accessor functions (fb_ev_current_desktop, etc.) provide lazy-fetching
 * of EWMH values with caching inside the FbEv instance.  They should be
 * called from signal callbacks or at startup to read current state.
 *
 * NOTE: The file header says "fb-background-monitor.h" — copy-paste error;
 * this is the FbEv header.
 */

/* FIXME: this needs to be made multiscreen aware
 *        panel_bg_get should take
 *        a GdkScreen argument.
 */

#ifndef __FB_EV_H__
#define __FB_EV_H__

#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>

/*
 * GObject type macros for FbEv.
 *
 * FB_TYPE_EV         - the GType for FbEv; use in g_object_new() etc.
 * FB_EV(o)           - cast a GObject pointer to FbEv* (with type check in debug).
 * FB_EV_CLASS(k)     - cast a GObjectClass pointer to FbEvClass*.
 * FB_IS_EV(o)        - runtime type check; returns TRUE if o is an FbEv.
 * FB_IS_EV_CLASS(k)  - runtime type check on the class pointer.
 * FB_EV_GET_CLASS(o) - retrieve the FbEvClass* from an instance pointer.
 */
#define FB_TYPE_EV         (fb_ev_get_type ())
#define FB_EV(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o),      \
				       FB_TYPE_EV,        \
				       FbEv))
#define FB_EV_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k),         \
				       FB_TYPE_EV,        \
				       FbEvClass))
#define FB_IS_EV(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o),      \
				       FB_TYPE_EV))
#define FB_IS_EV_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k),         \
				       FB_TYPE_EV))
#define FB_EV_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o),       \
				       FB_TYPE_EV,        \
				       FbEvClass))

/*
 * Opaque type declarations.
 * Struct definitions are private to ev.c; external code uses the API below.
 */
typedef struct _FbEvClass FbEvClass;
typedef struct _FbEv      FbEv;

/*
 * Signal index enum for FbEv.
 *
 * These values are used as indices into the internal signals[] array and
 * as the `signal` argument to fb_ev_trigger().  They correspond to EWMH
 * root-window properties that the panel monitors:
 *
 *   EV_CURRENT_DESKTOP       - _NET_CURRENT_DESKTOP: active virtual desktop changed.
 *   EV_NUMBER_OF_DESKTOPS    - _NET_NUMBER_OF_DESKTOPS: desktop count changed.
 *   EV_DESKTOP_NAMES         - _NET_DESKTOP_NAMES: desktop names changed.
 *   EV_ACTIVE_WINDOW         - _NET_ACTIVE_WINDOW: focused window changed.
 *   EV_CLIENT_LIST_STACKING  - _NET_CLIENT_LIST_STACKING: stacking order changed.
 *   EV_CLIENT_LIST           - _NET_CLIENT_LIST: managed window list changed.
 *   EV_LAST_SIGNAL           - sentinel; equals the total number of signals.
 *
 * IMPORTANT: The order of values in this enum must exactly match the order
 * of g_signal_new() calls in fb_ev_class_init() in ev.c, because the
 * signals[] array is indexed by these values.
 */
enum {
    EV_CURRENT_DESKTOP,        // index 0: _NET_CURRENT_DESKTOP changed
    EV_NUMBER_OF_DESKTOPS,     // index 1: _NET_NUMBER_OF_DESKTOPS changed
    EV_DESKTOP_NAMES,          // index 2: _NET_DESKTOP_NAMES changed
    EV_ACTIVE_WINDOW,          // index 3: _NET_ACTIVE_WINDOW changed
    EV_CLIENT_LIST_STACKING,   // index 4: _NET_CLIENT_LIST_STACKING changed
    EV_CLIENT_LIST,            // index 5: _NET_CLIENT_LIST changed
    EV_LAST_SIGNAL             // sentinel: total count of signals
};

/*
 * fb_ev_get_type - return the GType for the FbEv class.
 *
 * Registers the type on the first call.  Used by the FB_TYPE_EV and FB_EV()
 * macros; should not normally be called directly.
 */
GType fb_ev_get_type       (void);

/*
 * fb_ev_new - allocate and return a new FbEv instance.
 *
 * Returns: a new FbEv with reference count 1.
 *          Caller must eventually call g_object_unref().
 *
 * Typically the panel creates one global FbEv and keeps it alive for the
 * process lifetime; plugins connect to its signals.
 */
FbEv *fb_ev_new(void);

/*
 * fb_ev_notify_changed_ev - declared in header but NOT defined anywhere.
 *
 * This function is declared here but has no implementation in ev.c or
 * any other known translation unit in fbpanel.  Calling it will cause a
 * linker error (undefined reference).  It appears to be a stub that was
 * never implemented.  Compare with fb_bg_notify_changed_bg() in bg.c
 * which IS implemented — this seems to be an oversight.
 *
 * NOTE: fb_ev_trigger() is the correct function to use for emitting signals.
 */
void fb_ev_notify_changed_ev(FbEv *ev);  // WARNING: not implemented anywhere

/*
 * fb_ev_trigger - emit an EWMH signal on an FbEv instance.
 *
 * Parameters:
 *   ev     - a valid FbEv instance.
 *   signal - one of the EV_* enum values; must be in [0, EV_LAST_SIGNAL).
 *            g_assert() will abort if out of range (debug builds only;
 *            release builds may have undefined behaviour).
 *
 * Called by the panel's X event handler when a PropertyNotify is received
 * for a monitored EWMH atom.  Fires the class handler first (cache
 * invalidation), then all connected user callbacks.
 */
void fb_ev_trigger(FbEv *ev, int signal);


/*
 * fb_ev_current_desktop - return the currently active virtual desktop index.
 *
 * Parameters:
 *   ev - a valid FbEv instance.
 *
 * Returns: 0-based desktop index.  Returns 0 if _NET_CURRENT_DESKTOP is not set.
 *
 * Lazy-fetches and caches the value from _NET_CURRENT_DESKTOP on the root
 * window.  The cache is invalidated when EV_CURRENT_DESKTOP is triggered.
 */
int fb_ev_current_desktop(FbEv *ev);

/*
 * fb_ev_number_of_desktops - return the total number of virtual desktops.
 *
 * Parameters:
 *   ev - a valid FbEv instance.
 *
 * Returns: desktop count.  Returns 0 if _NET_NUMBER_OF_DESKTOPS is not set.
 *
 * Lazy-fetches and caches the value from _NET_NUMBER_OF_DESKTOPS.
 * The cache is invalidated when EV_NUMBER_OF_DESKTOPS is triggered.
 */
int fb_ev_number_of_desktops(FbEv *ev);

/*
 * fb_ev_active_window - return the currently active (focused) window.
 *
 * Parameters:
 *   ev - a valid FbEv instance.
 *
 * Returns: X11 Window ID of the active window, or None if not set.
 *
 * NOTE: This function is declared here and in ev.c but has NO IMPLEMENTATION
 * anywhere in the visible source.  Calling it will cause a linker error.
 * The cache field (ev->active_window) and the invalidation handler
 * (ev_active_window) exist, but the lazy-fetch body was never written.
 */
Window fb_ev_active_window(FbEv *ev);  // WARNING: implementation is missing

/*
 * fb_ev_client_list - return the list of all managed client windows.
 *
 * Parameters:
 *   ev - a valid FbEv instance.
 *
 * Returns: pointer to an array of X11 Window IDs from _NET_CLIENT_LIST,
 *          or NULL if not set.  The array is owned by FbEv; do NOT XFree() it.
 *          The pointer becomes invalid after the next EV_CLIENT_LIST trigger.
 *
 * NOTE: This function is declared here and in ev.c but has NO IMPLEMENTATION
 * anywhere in the visible source.  Calling it will cause a linker error.
 */
Window *fb_ev_client_list(FbEv *ev);  // WARNING: implementation is missing

/*
 * fb_ev_client_list_stacking - return client windows in stacking order.
 *
 * Parameters:
 *   ev - a valid FbEv instance.
 *
 * Returns: pointer to an array of X11 Window IDs from _NET_CLIENT_LIST_STACKING,
 *          or NULL if not set.  The array is owned by FbEv; do NOT XFree() it.
 *          The pointer becomes invalid after the next EV_CLIENT_LIST_STACKING trigger.
 *
 * NOTE: This function is declared here and in ev.c but has NO IMPLEMENTATION
 * anywhere in the visible source.  Calling it will cause a linker error.
 */
Window *fb_ev_client_list_stacking(FbEv *ev);  // WARNING: implementation is missing


#endif /* __FB_EV_H__ */
