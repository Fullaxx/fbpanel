#ifndef EWMH_H
#define EWMH_H

/**
 * @file
 * @brief X11 Atom table and EWMH/ICCCM property helpers.
 *
 * Declares the global X11 Atom handles that are interned once at startup
 * by resolve_atoms(), and the functions that read EWMH/ICCCM window
 * properties from those atoms.
 *
 * @note Call resolve_atoms() (via fb_init()) before using any atom or
 *       function declared here.
 */

#include <X11/Xatom.h>
#include <glib.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>

#include "panel.h"    /* net_wm_state, net_wm_window_type */

/* -----------------------------------------------------------------------
 * Global X11 Atom handles.
 * Interned once at startup by resolve_atoms(); do NOT use before fb_init().
 * ----------------------------------------------------------------------- */

/* Encoding atom */
extern Atom a_UTF8_STRING;
extern Atom a_XROOTPMAP_ID;

/* ICCCM / legacy WM atoms */
extern Atom a_WM_STATE;
extern Atom a_WM_CLASS;
extern Atom a_WM_DELETE_WINDOW;
extern Atom a_WM_PROTOCOLS;

/* EWMH (_NET_*) atoms */
extern Atom a_NET_WORKAREA;
extern Atom a_NET_CLIENT_LIST;
extern Atom a_NET_CLIENT_LIST_STACKING;
extern Atom a_NET_NUMBER_OF_DESKTOPS;
extern Atom a_NET_CURRENT_DESKTOP;
extern Atom a_NET_DESKTOP_NAMES;
extern Atom a_NET_DESKTOP_GEOMETRY;
extern Atom a_NET_ACTIVE_WINDOW;
extern Atom a_NET_CLOSE_WINDOW;
extern Atom a_NET_SUPPORTED;
extern Atom a_NET_WM_DESKTOP;
extern Atom a_NET_WM_STATE;
extern Atom a_NET_WM_STATE_SKIP_TASKBAR;
extern Atom a_NET_WM_STATE_SKIP_PAGER;
extern Atom a_NET_WM_STATE_STICKY;
extern Atom a_NET_WM_STATE_HIDDEN;
extern Atom a_NET_WM_STATE_SHADED;
extern Atom a_NET_WM_STATE_ABOVE;
extern Atom a_NET_WM_STATE_BELOW;
extern Atom a_NET_WM_WINDOW_TYPE;
extern Atom a_NET_WM_WINDOW_TYPE_DESKTOP;
extern Atom a_NET_WM_WINDOW_TYPE_DOCK;
extern Atom a_NET_WM_WINDOW_TYPE_TOOLBAR;
extern Atom a_NET_WM_WINDOW_TYPE_MENU;
extern Atom a_NET_WM_WINDOW_TYPE_UTILITY;
extern Atom a_NET_WM_WINDOW_TYPE_SPLASH;
extern Atom a_NET_WM_WINDOW_TYPE_DIALOG;
extern Atom a_NET_WM_WINDOW_TYPE_NORMAL;
extern Atom a_NET_WM_NAME;
extern Atom a_NET_WM_VISIBLE_NAME;
extern Atom a_NET_WM_STRUT;
extern Atom a_NET_WM_STRUT_PARTIAL;
extern Atom a_NET_WM_ICON;
extern Atom a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR;

/* -----------------------------------------------------------------------
 * Initialisation
 * ----------------------------------------------------------------------- */

/** @brief Intern all atoms above with the X server. Called once from fb_init(). */
void resolve_atoms(void);

/* -----------------------------------------------------------------------
 * X11 client message senders
 * ----------------------------------------------------------------------- */

/**
 * @brief Send a generic 32-bit ClientMessage event to a window.
 *
 * Wraps XSendEvent() with `SubstructureNotifyMask | SubstructureRedirectMask`
 * on the root window, per the ICCCM/EWMH convention for WM-targeted messages.
 *
 * @param win  Destination X11 window.
 * @param type Atom identifying the message type (goes in `xclient.message_type`).
 * @param l0   First data long (`xclient.data.l[0]`).
 * @param l1   Second data long.
 * @param l2   Third data long.
 * @param l3   Fourth data long.
 * @param l4   Fifth data long.
 */
extern void Xclimsg(Window win, long type,
                    long l0, long l1, long l2, long l3, long l4);

/**
 * @brief Send a ClientMessage whose single data word is an Atom.
 *
 * Convenience wrapper over Xclimsg() for the common WM-protocol case of
 * sending one atom argument (e.g. `WM_PROTOCOLS`/`WM_DELETE_WINDOW`).
 *
 * @param win  Destination X11 window.
 * @param type Atom identifying the message type.
 * @param arg  Atom carried as the message's data.
 */
void Xclimsgwm(Window win, Atom type, Atom arg);

/* -----------------------------------------------------------------------
 * Raw property readers
 * ----------------------------------------------------------------------- */

/**
 * @brief Read a raw X11 window property of a given type.
 *
 * @param win    The window to query.
 * @param prop   Atom identifying the property.
 * @param type   Expected property type atom (e.g. `XA_CARDINAL`, `XA_ATOM`).
 * @param nitems Out parameter: number of items read.
 * @return Pointer to the property data (caller must XFree() it), or NULL
 *         if the property is absent or of the wrong type.
 */
extern void *get_xaproperty(Window win, Atom prop, Atom type, int *nitems);

/**
 * @brief Read a text property (e.g. `WM_NAME`) as a newly-allocated string.
 *
 * @param win  The window to query.
 * @param atom Atom identifying the text property.
 * @return A newly allocated string, or NULL if the property is absent.
 */
char *get_textproperty(Window win, Atom atom);

/**
 * @brief Read a single UTF-8 string property (e.g. `_NET_WM_NAME`).
 *
 * @param win  The window to query.
 * @param atom Atom identifying the UTF-8 property.
 * @return Newly allocated UTF-8 string, or NULL if absent.
 */
void *get_utf8_property(Window win, Atom atom);

/**
 * @brief Read a UTF-8 string-list property (e.g. `_NET_DESKTOP_NAMES`).
 *
 * @param win   The window to query.
 * @param atom  Atom identifying the UTF-8 string-list property.
 * @param count Out parameter: number of strings in the returned array.
 * @return Newly allocated, NULL-safe array of newly allocated strings, or
 *         NULL if absent.
 */
char **get_utf8_property_list(Window win, Atom atom, int *count);

/* -----------------------------------------------------------------------
 * EWMH convenience wrappers
 * ----------------------------------------------------------------------- */

/** @brief Read `_NET_NUMBER_OF_DESKTOPS` from the root window. @return Desktop count, or 0 if unset. */
extern guint get_net_number_of_desktops(void);

/** @brief Read `_NET_CURRENT_DESKTOP` from the root window. @return 0-based active desktop index, or 0 if unset. */
extern guint get_net_current_desktop(void);

/** @brief Read `_NET_WM_DESKTOP` for a window. @param win The window to query. @return Desktop index the window is on. */
extern guint get_net_wm_desktop(Window win);

/** @brief Decode `_NET_WM_STATE` for a window into a ::net_wm_state bitfield. @param win The window to query. @param nws Out parameter: populated bitfield. */
extern void  get_net_wm_state(Window win, net_wm_state *nws);

/** @brief Decode `_NET_WM_WINDOW_TYPE` for a window into a ::net_wm_window_type bitfield. @param win The window to query. @param nwwt Out parameter: populated bitfield. */
extern void  get_net_wm_window_type(Window win, net_wm_window_type *nwwt);

#endif /* EWMH_H */
