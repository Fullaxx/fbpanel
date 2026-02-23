/*
 * ewmh.c -- X11 Atom table and EWMH/ICCCM property helpers.
 *
 * Owns all global X11 Atom handles and implements the low-level property
 * readers used throughout fbpanel and its plugins.
 *
 * Extracted from misc.c.  No logic changes.
 */

#include <X11/Xatom.h>
#include <X11/cursorfont.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <string.h>

#include <glib.h>

#include "ewmh.h"

//#define DEBUGPRN
#include "dbg.h"

/* -----------------------------------------------------------------------
 * Global X11 Atom table
 * All atoms are interned once at startup by resolve_atoms().
 * ----------------------------------------------------------------------- */

Atom a_UTF8_STRING;
Atom a_XROOTPMAP_ID;

Atom a_WM_STATE;
Atom a_WM_CLASS;
Atom a_WM_DELETE_WINDOW;
Atom a_WM_PROTOCOLS;

Atom a_NET_WORKAREA;
Atom a_NET_CLIENT_LIST;
Atom a_NET_CLIENT_LIST_STACKING;
Atom a_NET_NUMBER_OF_DESKTOPS;
Atom a_NET_CURRENT_DESKTOP;
Atom a_NET_DESKTOP_NAMES;
Atom a_NET_DESKTOP_GEOMETRY;
Atom a_NET_ACTIVE_WINDOW;
Atom a_NET_CLOSE_WINDOW;
Atom a_NET_SUPPORTED;
Atom a_NET_WM_DESKTOP;
Atom a_NET_WM_STATE;
Atom a_NET_WM_STATE_SKIP_TASKBAR;
Atom a_NET_WM_STATE_SKIP_PAGER;
Atom a_NET_WM_STATE_STICKY;
Atom a_NET_WM_STATE_HIDDEN;
Atom a_NET_WM_STATE_SHADED;
Atom a_NET_WM_STATE_ABOVE;
Atom a_NET_WM_STATE_BELOW;
Atom a_NET_WM_WINDOW_TYPE;
Atom a_NET_WM_WINDOW_TYPE_DESKTOP;
Atom a_NET_WM_WINDOW_TYPE_DOCK;
Atom a_NET_WM_WINDOW_TYPE_TOOLBAR;
Atom a_NET_WM_WINDOW_TYPE_MENU;
Atom a_NET_WM_WINDOW_TYPE_UTILITY;
Atom a_NET_WM_WINDOW_TYPE_SPLASH;
Atom a_NET_WM_WINDOW_TYPE_DIALOG;
Atom a_NET_WM_WINDOW_TYPE_NORMAL;
Atom a_NET_WM_NAME;
Atom a_NET_WM_VISIBLE_NAME;
Atom a_NET_WM_STRUT;
Atom a_NET_WM_STRUT_PARTIAL;
Atom a_NET_WM_ICON;
Atom a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR;

/*
 * resolve_atoms - Intern all required X11 atoms with the X server.
 *
 * Called once from fb_init() before any X11 property reads or writes.
 * XInternAtom with only_if_exists=False guarantees atom creation.
 */
void resolve_atoms(void)
{
    ENTER;

    a_UTF8_STRING                = XInternAtom(GDK_DISPLAY(), "UTF8_STRING", False);
    a_XROOTPMAP_ID               = XInternAtom(GDK_DISPLAY(), "_XROOTPMAP_ID", False);
    a_WM_STATE                   = XInternAtom(GDK_DISPLAY(), "WM_STATE", False);
    a_WM_CLASS                   = XInternAtom(GDK_DISPLAY(), "WM_CLASS", False);
    a_WM_DELETE_WINDOW           = XInternAtom(GDK_DISPLAY(), "WM_DELETE_WINDOW", False);
    a_WM_PROTOCOLS               = XInternAtom(GDK_DISPLAY(), "WM_PROTOCOLS", False);
    a_NET_WORKAREA               = XInternAtom(GDK_DISPLAY(), "_NET_WORKAREA", False);
    a_NET_CLIENT_LIST            = XInternAtom(GDK_DISPLAY(), "_NET_CLIENT_LIST", False);
    a_NET_CLIENT_LIST_STACKING   = XInternAtom(GDK_DISPLAY(), "_NET_CLIENT_LIST_STACKING", False);
    a_NET_NUMBER_OF_DESKTOPS     = XInternAtom(GDK_DISPLAY(), "_NET_NUMBER_OF_DESKTOPS", False);
    a_NET_CURRENT_DESKTOP        = XInternAtom(GDK_DISPLAY(), "_NET_CURRENT_DESKTOP", False);
    a_NET_DESKTOP_NAMES          = XInternAtom(GDK_DISPLAY(), "_NET_DESKTOP_NAMES", False);
    a_NET_DESKTOP_GEOMETRY       = XInternAtom(GDK_DISPLAY(), "_NET_DESKTOP_GEOMETRY", False);
    a_NET_ACTIVE_WINDOW          = XInternAtom(GDK_DISPLAY(), "_NET_ACTIVE_WINDOW", False);
    a_NET_SUPPORTED              = XInternAtom(GDK_DISPLAY(), "_NET_SUPPORTED", False);
    a_NET_WM_DESKTOP             = XInternAtom(GDK_DISPLAY(), "_NET_WM_DESKTOP", False);
    a_NET_WM_STATE               = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE", False);
    a_NET_WM_STATE_SKIP_TASKBAR  = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_SKIP_TASKBAR", False);
    a_NET_WM_STATE_SKIP_PAGER    = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_SKIP_PAGER", False);
    a_NET_WM_STATE_STICKY        = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_STICKY", False);
    a_NET_WM_STATE_HIDDEN        = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_HIDDEN", False);
    a_NET_WM_STATE_SHADED        = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_SHADED", False);
    a_NET_WM_STATE_ABOVE         = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_ABOVE", False);
    a_NET_WM_STATE_BELOW         = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_BELOW", False);
    a_NET_WM_WINDOW_TYPE         = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE", False);
    a_NET_WM_WINDOW_TYPE_DESKTOP = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    a_NET_WM_WINDOW_TYPE_DOCK    = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_DOCK", False);
    a_NET_WM_WINDOW_TYPE_TOOLBAR = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    a_NET_WM_WINDOW_TYPE_MENU    = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_MENU", False);
    a_NET_WM_WINDOW_TYPE_UTILITY = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_UTILITY", False);
    a_NET_WM_WINDOW_TYPE_SPLASH  = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_SPLASH", False);
    a_NET_WM_WINDOW_TYPE_DIALOG  = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_DIALOG", False);
    a_NET_WM_WINDOW_TYPE_NORMAL  = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_NORMAL", False);
    a_NET_WM_NAME                = XInternAtom(GDK_DISPLAY(), "_NET_WM_NAME", False);
    a_NET_WM_VISIBLE_NAME        = XInternAtom(GDK_DISPLAY(), "_NET_WM_VISIBLE_NAME", False);
    a_NET_WM_STRUT               = XInternAtom(GDK_DISPLAY(), "_NET_WM_STRUT", False);
    a_NET_WM_STRUT_PARTIAL       = XInternAtom(GDK_DISPLAY(), "_NET_WM_STRUT_PARTIAL", False);
    a_NET_WM_ICON                = XInternAtom(GDK_DISPLAY(), "_NET_WM_ICON", False);
    a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR
                                 = XInternAtom(GDK_DISPLAY(), "_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR", False);

    RET();
}

/*
 * Xclimsg - Send an EWMH client message to the root window.
 *
 * Constructs a 32-bit XClientMessageEvent and broadcasts it to the root
 * window with SubstructureNotify|SubstructureRedirect masks (EWMH standard).
 */
void
Xclimsg(Window win, long type, long l0, long l1, long l2, long l3, long l4)
{
    XClientMessageEvent xev;

    xev.type = ClientMessage;
    xev.window = win;
    xev.send_event = True;
    xev.display = gdk_display;
    xev.message_type = type;
    xev.format = 32;
    xev.data.l[0] = l0;
    xev.data.l[1] = l1;
    xev.data.l[2] = l2;
    xev.data.l[3] = l3;
    xev.data.l[4] = l4;
    XSendEvent(GDK_DISPLAY(), GDK_ROOT_WINDOW(), False,
          (SubstructureNotifyMask | SubstructureRedirectMask),
          (XEvent *) & xev);
}

/*
 * Xclimsgwm - Send a WM_PROTOCOLS client message directly to a window.
 *
 * Used for ICCCM-style WM protocol messages (e.g. WM_DELETE_WINDOW).
 */
void
Xclimsgwm(Window win, Atom type, Atom arg)
{
    XClientMessageEvent xev;

    xev.type = ClientMessage;
    xev.window = win;
    xev.message_type = type;
    xev.format = 32;
    xev.data.l[0] = arg;
    xev.data.l[1] = GDK_CURRENT_TIME;
    XSendEvent(GDK_DISPLAY(), win, False, 0L, (XEvent *) &xev);
}

/*
 * get_utf8_property - Read a UTF8_STRING X11 property from a window.
 *
 * Returns a newly-allocated NUL-terminated UTF-8 string, or NULL.
 * Caller must free with g_free().
 */
void *
get_utf8_property(Window win, Atom atom)
{
    Atom type;
    int format;
    gulong nitems;
    gulong bytes_after;
    gchar  *retval;
    int result;
    guchar *tmp = NULL;

    type = None;
    retval = NULL;
    result = XGetWindowProperty(GDK_DISPLAY(), win, atom, 0, G_MAXLONG, False,
          a_UTF8_STRING, &type, &format, &nitems,
          &bytes_after, &tmp);
    if (result != Success)
        return NULL;
    if (tmp) {
        if (type == a_UTF8_STRING && format == 8 && nitems != 0)
            retval = g_strndup((gchar *)tmp, nitems);
        XFree(tmp);
    }
    return retval;
}

/*
 * get_utf8_property_list - Read a multi-string UTF8_STRING X11 property.
 *
 * Splits NUL-separated strings into a NULL-terminated array.
 * Returns a newly-allocated array; caller must free with g_strfreev().
 */
char **
get_utf8_property_list(Window win, Atom atom, int *count)
{
    Atom type;
    int format, i;
    gulong nitems;
    gulong bytes_after;
    gchar *s, **retval = NULL;
    int result;
    guchar *tmp = NULL;

    *count = 0;
    result = XGetWindowProperty(GDK_DISPLAY(), win, atom, 0, G_MAXLONG, False,
          a_UTF8_STRING, &type, &format, &nitems,
          &bytes_after, &tmp);
    if (result != Success || type != a_UTF8_STRING || tmp == NULL)
        return NULL;

    if (nitems) {
        gchar *val = (gchar *) tmp;
        DBG("res=%d(%d) nitems=%d val=%s\n", result, Success, nitems, val);

        for (i = 0; i < nitems; i++) {
            if (!val[i])
                (*count)++;
        }

        retval = g_new0(char*, *count + 2);

        for (i = 0, s = val; i < *count; i++, s = s + strlen(s) + 1) {
            retval[i] = g_strdup(s);
        }

        if (val[nitems-1]) {
            result = nitems - (s - val);
            DBG("val does not ends by 0, moving last %d bytes\n", result);
            memmove(s - 1, s, result);
            val[nitems-1] = 0;
            DBG("s=%s\n", s - 1);
            retval[i] = g_strdup(s - 1);
            (*count)++;
        }
    }
    XFree(tmp);

    return retval;
}

/*
 * get_xaproperty - Generic X11 window property reader.
 *
 * Returns a pointer to Xlib-allocated property data, or NULL.
 * Caller MUST free with XFree(), not g_free().
 */
void *
get_xaproperty(Window win, Atom prop, Atom type, int *nitems)
{
    Atom type_ret;
    int format_ret;
    unsigned long items_ret;
    unsigned long after_ret;
    unsigned char *prop_data;

    ENTER;
    prop_data = NULL;
    if (XGetWindowProperty(GDK_DISPLAY(), win, prop, 0, 0x7fffffff, False,
              type, &type_ret, &format_ret, &items_ret,
              &after_ret, &prop_data) != Success)
        RET(NULL);
    DBG("win=%x prop=%d type=%d rtype=%d rformat=%d nitems=%d\n", win, prop,
            type, type_ret, format_ret, items_ret);

    if (nitems)
        *nitems = items_ret;
    RET(prop_data);
}

/*
 * text_property_to_utf8 - Convert an XTextProperty to a UTF-8 string.
 *
 * Returns a newly-allocated string; caller must free with g_free().
 */
static char *
text_property_to_utf8(const XTextProperty *prop)
{
    char **list;
    int count;
    char *retval;

    ENTER;
    list = NULL;
    count = gdk_text_property_to_utf8_list(gdk_x11_xatom_to_atom(prop->encoding),
                                            prop->format,
                                            prop->value,
                                            prop->nitems,
                                            &list);

    DBG("count=%d\n", count);
    if (count == 0)
        return NULL;

    retval = list[0];
    list[0] = g_strdup("");
    g_strfreev(list);

    RET(retval);
}

/*
 * get_textproperty - Read an ICCCM XTextProperty and convert to UTF-8.
 *
 * Returns a newly-allocated string; caller must free with g_free().
 */
char *
get_textproperty(Window win, Atom atom)
{
    XTextProperty text_prop;
    char *retval;

    ENTER;
    if (XGetTextProperty(GDK_DISPLAY(), win, &text_prop, atom)) {
        DBG("format=%d enc=%d nitems=%d value=%s   \n",
              text_prop.format,
              text_prop.encoding,
              text_prop.nitems,
              text_prop.value);
        retval = text_property_to_utf8(&text_prop);
        if (text_prop.nitems > 0)
            XFree(text_prop.value);
        RET(retval);
    }
    RET(NULL);
}

/*
 * get_net_number_of_desktops - Query the current desktop count from the WM.
 *
 * Returns the _NET_NUMBER_OF_DESKTOPS value, or 0 if not set.
 */
guint
get_net_number_of_desktops(void)
{
    guint desknum;
    guint32 *data;

    ENTER;
    data = get_xaproperty(GDK_ROOT_WINDOW(), a_NET_NUMBER_OF_DESKTOPS,
          XA_CARDINAL, 0);
    if (!data)
        RET(0);

    desknum = *data;
    XFree(data);
    RET(desknum);
}

/*
 * get_net_current_desktop - Query the currently active virtual desktop index.
 *
 * Returns the _NET_CURRENT_DESKTOP value, or 0 on failure.
 */
guint
get_net_current_desktop(void)
{
    guint desk;
    guint32 *data;

    ENTER;
    data = get_xaproperty(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, XA_CARDINAL, 0);
    if (!data)
        RET(0);

    desk = *data;
    XFree(data);
    RET(desk);
}

/*
 * get_net_wm_desktop - Query which virtual desktop a specific window is on.
 *
 * Returns the _NET_WM_DESKTOP value (0xFFFFFFFF = sticky), or 0 on failure.
 */
guint
get_net_wm_desktop(Window win)
{
    guint desk = 0;
    guint *data;

    ENTER;
    data = get_xaproperty(win, a_NET_WM_DESKTOP, XA_CARDINAL, 0);
    if (data) {
        desk = *data;
        XFree(data);
    } else
        DBG("can't get desktop num for win 0x%lx", win);
    RET(desk);
}

/*
 * get_net_wm_state - Populate a net_wm_state struct from a window's EWMH state.
 *
 * Decodes _NET_WM_STATE atom array into bitfield members of @nws.
 * @nws is always zeroed before population.
 */
void
get_net_wm_state(Window win, net_wm_state *nws)
{
    Atom *state;
    int num3;

    ENTER;
    bzero(nws, sizeof(*nws));
    if (!(state = get_xaproperty(win, a_NET_WM_STATE, XA_ATOM, &num3)))
        RET();

    DBG("%x: netwm state = { ", (unsigned int)win);
    while (--num3 >= 0) {
        if (state[num3] == a_NET_WM_STATE_SKIP_PAGER) {
            DBGE("NET_WM_STATE_SKIP_PAGER ");
            nws->skip_pager = 1;
        } else if (state[num3] == a_NET_WM_STATE_SKIP_TASKBAR) {
            DBGE("NET_WM_STATE_SKIP_TASKBAR ");
            nws->skip_taskbar = 1;
        } else if (state[num3] == a_NET_WM_STATE_STICKY) {
            DBGE("NET_WM_STATE_STICKY ");
            nws->sticky = 1;
        } else if (state[num3] == a_NET_WM_STATE_HIDDEN) {
            DBGE("NET_WM_STATE_HIDDEN ");
            nws->hidden = 1;
        } else if (state[num3] == a_NET_WM_STATE_SHADED) {
            DBGE("NET_WM_STATE_SHADED ");
            nws->shaded = 1;
        } else {
            DBGE("... ");
        }
    }
    XFree(state);
    DBGE("}\n");
    RET();
}

/*
 * get_net_wm_window_type - Populate a net_wm_window_type struct from a window's type.
 *
 * Decodes _NET_WM_WINDOW_TYPE atom array into bitfield members of @nwwt.
 * @nwwt is always zeroed before population.
 */
void
get_net_wm_window_type(Window win, net_wm_window_type *nwwt)
{
    Atom *state;
    int num3;

    ENTER;
    bzero(nwwt, sizeof(*nwwt));
    if (!(state = get_xaproperty(win, a_NET_WM_WINDOW_TYPE, XA_ATOM, &num3)))
        RET();

    DBG("%x: netwm state = { ", (unsigned int)win);
    while (--num3 >= 0) {
        if (state[num3] == a_NET_WM_WINDOW_TYPE_DESKTOP) {
            DBG("NET_WM_WINDOW_TYPE_DESKTOP ");
            nwwt->desktop = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_DOCK) {
            DBG("NET_WM_WINDOW_TYPE_DOCK ");
            nwwt->dock = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_TOOLBAR) {
            DBG("NET_WM_WINDOW_TYPE_TOOLBAR ");
            nwwt->toolbar = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_MENU) {
            DBG("NET_WM_WINDOW_TYPE_MENU ");
            nwwt->menu = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_UTILITY) {
            DBG("NET_WM_WINDOW_TYPE_UTILITY ");
            nwwt->utility = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_SPLASH) {
            DBG("NET_WM_WINDOW_TYPE_SPLASH ");
            nwwt->splash = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_DIALOG) {
            DBG("NET_WM_WINDOW_TYPE_DIALOG ");
            nwwt->dialog = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_NORMAL) {
            DBG("NET_WM_WINDOW_TYPE_NORMAL ");
            nwwt->normal = 1;
        } else {
            DBG("... ");
        }
    }
    XFree(state);
    DBG("}\n");
    RET();
}
