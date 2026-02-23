#ifndef EWMH_H
#define EWMH_H

/*
 * ewmh.h -- X11 Atom table and EWMH/ICCCM property helpers.
 *
 * Declares the global X11 Atom handles that are interned once at startup
 * by resolve_atoms(), and the functions that read EWMH/ICCCM window
 * properties from those atoms.
 *
 * Call resolve_atoms() (via fb_init()) before using any atom or function
 * declared here.
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

/* Intern all atoms above with the X server.  Called once from fb_init(). */
void resolve_atoms(void);

/* -----------------------------------------------------------------------
 * X11 client message senders
 * ----------------------------------------------------------------------- */

extern void Xclimsg(Window win, long type,
                    long l0, long l1, long l2, long l3, long l4);
void Xclimsgwm(Window win, Atom type, Atom arg);

/* -----------------------------------------------------------------------
 * Raw property readers
 * ----------------------------------------------------------------------- */

extern void *get_xaproperty(Window win, Atom prop, Atom type, int *nitems);
char *get_textproperty(Window win, Atom prop);
void *get_utf8_property(Window win, Atom atom);
char **get_utf8_property_list(Window win, Atom atom, int *count);

/* -----------------------------------------------------------------------
 * EWMH convenience wrappers
 * ----------------------------------------------------------------------- */

extern guint get_net_number_of_desktops(void);
extern guint get_net_current_desktop(void);
extern guint get_net_wm_desktop(Window win);
extern void  get_net_wm_state(Window win, net_wm_state *nws);
extern void  get_net_wm_window_type(Window win, net_wm_window_type *nwwt);

#endif /* EWMH_H */
