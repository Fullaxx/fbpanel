
/*
 * misc.c - Central utility module for fbpanel
 *
 * This file is the backbone of fbpanel's utility layer.  It owns:
 *   - All global X11 Atom handles (intern'd once at startup via resolve_atoms).
 *   - EWMH/NetWM property readers (get_utf8_property*, get_xaproperty, etc.).
 *   - Panel geometry calculation (calculate_width, calculate_position).
 *   - Pixbuf / image / button widget factory (fb_pixbuf_new, fb_image_new,
 *     fb_button_new) with icon-theme-change tracking and hover animation.
 *   - Miscellaneous helpers: str2num, num2str, expand_tilda, menu_pos,
 *     gcolor2rgb24, gdk_color_to_RRGGBB, indent.
 *
 * Memory ownership conventions used throughout this file:
 *   - Functions whose names end with "_new" or "_dup" return a newly allocated
 *     object that the caller owns and must free (g_free / g_object_unref).
 *   - Property data returned by XGetWindowProperty must be freed with XFree(),
 *     not g_free().  Strings derived from such data are copied with g_strdup /
 *     g_strndup and must be freed with g_free().
 *   - GtkWidget ownership follows the standard GTK2 widget tree: adding a
 *     widget to a container transfers ownership to the container.  Widgets not
 *     yet in a container must be destroyed explicitly.
 */

#include <X11/Xatom.h>
#include <X11/cursorfont.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "misc.h"
#include "gtkbgbox.h"

//#define DEBUGPRN
#include "dbg.h"

/* Global reference to the single panel instance.  Defined in panel.c and
 * used here by menu_pos() to query the panel's geometry.
 * ISSUE: This hard-codes single-panel support; multi-panel setups would
 * require passing the panel explicitly. */
extern panel *the_panel;

/* Default GTK icon theme, obtained once in fb_init().
 * Not ref-counted; GtkIconTheme is a singleton managed by GTK itself. */
GtkIconTheme *icon_theme;

/* ---------------------------------------------------------------------------
 * Global X11 Atom table
 * All atoms are interned once at startup by resolve_atoms().
 * Using global variables rather than a struct is a deliberate simplicity
 * trade-off; it makes the atoms directly accessible from any translation unit
 * that includes misc.h / panel.h.
 * --------------------------------------------------------------------------- */

/* Encoding atom – used when reading/writing UTF-8 window properties */
Atom a_UTF8_STRING;
/* Root window property set by certain wallpaper programs (e.g. Esetroot)
 * holding the Pixmap XID of the current background image */
Atom a_XROOTPMAP_ID;

/* ICCCM / legacy WM atoms */
Atom a_WM_STATE;            /* ICCCM window state property */
Atom a_WM_CLASS;            /* ICCCM class hint property */
Atom a_WM_DELETE_WINDOW;    /* WM_PROTOCOLS atom for delete-window */
Atom a_WM_PROTOCOLS;        /* WM_PROTOCOLS property */

/* EWMH (_NET_*) atoms – Extended Window Manager Hints specification */
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
Atom a_NET_WM_DESKTOP;       /* NOTE: declared twice (see line ~66 below) */
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
/* BUG: a_NET_WM_DESKTOP is declared again here (duplicate global variable
 * declaration).  Both this and the one above refer to the same atom
 * "_NET_WM_DESKTOP", so there is no functional error, but it is confusing and
 * could cause linker warnings on strict compilers. */
Atom a_NET_WM_DESKTOP;
Atom a_NET_WM_NAME;
Atom a_NET_WM_VISIBLE_NAME;
Atom a_NET_WM_STRUT;
Atom a_NET_WM_STRUT_PARTIAL;
Atom a_NET_WM_ICON;
/* KDE-specific atom for system-tray embedding */
Atom a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR;

/* ---------------------------------------------------------------------------
 * Enum tables used by the xconf config reader / writer (xconf.c) and the
 * GTK config dialog (gconf_panel.c).  Each table is a NULL-terminated array
 * of {num, str, desc?} entries mapping integer constants to config-file
 * strings and (optionally) human-readable descriptions.
 * --------------------------------------------------------------------------- */

/* Maps ALLIGN_* constants to config-file tokens */
xconf_enum allign_enum[] = {
    { .num = ALLIGN_LEFT, .str = c_("left") },
    { .num = ALLIGN_RIGHT, .str = c_("right") },
    { .num = ALLIGN_CENTER, .str = c_("center")},
    { .num = 0, .str = NULL },  // sentinel
};
/* Maps EDGE_* constants to config-file tokens */
xconf_enum edge_enum[] = {
    { .num = EDGE_LEFT, .str = c_("left") },
    { .num = EDGE_RIGHT, .str = c_("right") },
    { .num = EDGE_TOP, .str = c_("top") },
    { .num = EDGE_BOTTOM, .str = c_("bottom") },
    { .num = 0, .str = NULL },  // sentinel
};
/* Maps WIDTH_* constants; .desc is shown in the GTK combo-box */
xconf_enum widthtype_enum[] = {
    { .num = WIDTH_REQUEST, .str = "request" , .desc = c_("dynamic") },
    { .num = WIDTH_PIXEL, .str = "pixel" , .desc = c_("pixels") },
    { .num = WIDTH_PERCENT, .str = "percent", .desc = c_("% of screen") },
    { .num = 0, .str = NULL },  // sentinel
};
/* Maps HEIGHT_* constants (only pixel height is currently supported) */
xconf_enum heighttype_enum[] = {
    { .num = HEIGHT_PIXEL, .str = c_("pixel") },
    { .num = 0, .str = NULL },  // sentinel
};
/* Maps boolean 0/1 to "false"/"true" for config file */
xconf_enum bool_enum[] = {
    { .num = 0, .str = "false" },
    { .num = 1, .str = "true" },
    { .num = 0, .str = NULL },  // sentinel
};
/* Maps POS_* constants (used by some plugins for icon position) */
xconf_enum pos_enum[] = {
    { .num = POS_NONE, .str = "none" },
    { .num = POS_START, .str = "start" },
    { .num = POS_END,  .str = "end" },
    { .num = 0, .str = NULL},  // sentinel
};
/* Maps LAYER_* stacking constants */
xconf_enum layer_enum[] = {
    { .num = LAYER_ABOVE, .str = c_("above") },
    { .num = LAYER_BELOW, .str = c_("below") },
    { .num = 0, .str = NULL},  // sentinel
};


/*
 * str2num - Look up a string in an xconf_enum table and return its integer value.
 *
 * @p      : Pointer to a NULL-terminated xconf_enum array.
 * @str    : The string to search for (case-insensitive comparison).
 * @defval : Default value returned when @str is not found in the table.
 *
 * Returns: The integer constant matching @str, or @defval if not found.
 *
 * Memory: No allocations; @str and @p are read-only.
 */
int
str2num(xconf_enum *p, gchar *str, int defval)
{
    ENTER;
    // Walk the table until the sentinel (str == NULL) is reached
    for (;p && p->str; p++) {
        if (!g_ascii_strcasecmp(str, p->str))
            RET(p->num);  // found – return the associated integer
    }
    RET(defval);  // not found – return the caller-supplied default
}

/*
 * num2str - Look up an integer in an xconf_enum table and return its string.
 *
 * @p      : Pointer to a NULL-terminated xconf_enum array.
 * @num    : Integer constant to look up.
 * @defval : Default string returned when @num is not found.
 *
 * Returns: The string matching @num (NOT a copy; points into the table),
 *          or @defval if not found.
 *
 * Memory: Caller must NOT free the returned string; it is a literal owned
 *         by the table.
 */
gchar *
num2str(xconf_enum *p, int num, gchar *defval)
{
    ENTER;
    // Walk the table until the sentinel (str == NULL) is reached
    for (;p && p->str; p++) {
        if (num == p->num)
            RET(p->str);  // found – return the associated string literal
    }
    RET(defval);  // not found – return the caller-supplied default
}


/*
 * resolve_atoms - Intern all required X11 atoms with the X server.
 *
 * Called once from fb_init() before any X11 property reads or writes.
 * XInternAtom with only_if_exists=False guarantees atom creation, so the
 * returned values are always valid (never None) after this call.
 *
 * ISSUE: GDK_DISPLAY() is a deprecated macro in later GTK2 builds; the
 *        preferred call is gdk_x11_get_default_xdisplay().
 *
 * ISSUE: a_NET_WM_STATE_SHADED is interned twice (lines 163 and 166).
 *        The second call is redundant but harmless.
 *
 * ISSUE: a_NET_WM_DESKTOP is interned twice (lines 157 and 177), mapping
 *        to the same atom name, because the global variable is declared twice.
 *        Redundant but harmless.
 *
 * No return value; fatal X11 errors would be caught by handle_error().
 */
void resolve_atoms()
{
    ENTER;

    // UTF-8 string encoding atom – used for _NET_WM_NAME, etc.
    a_UTF8_STRING                = XInternAtom(GDK_DISPLAY(), "UTF8_STRING", False);
    // Atom advertised by wallpaper-setting programs; used for pseudo-transparency
    a_XROOTPMAP_ID               = XInternAtom(GDK_DISPLAY(), "_XROOTPMAP_ID", False);
    // ICCCM legacy atoms
    a_WM_STATE                   = XInternAtom(GDK_DISPLAY(), "WM_STATE", False);
    a_WM_CLASS                   = XInternAtom(GDK_DISPLAY(), "WM_CLASS", False);
    a_WM_DELETE_WINDOW           = XInternAtom(GDK_DISPLAY(), "WM_DELETE_WINDOW", False);
    a_WM_PROTOCOLS               = XInternAtom(GDK_DISPLAY(), "WM_PROTOCOLS", False);
    // EWMH workarea / client list atoms
    a_NET_WORKAREA               = XInternAtom(GDK_DISPLAY(), "_NET_WORKAREA", False);
    a_NET_CLIENT_LIST            = XInternAtom(GDK_DISPLAY(), "_NET_CLIENT_LIST", False);
    a_NET_CLIENT_LIST_STACKING   = XInternAtom(GDK_DISPLAY(), "_NET_CLIENT_LIST_STACKING", False);
    // Desktop management atoms
    a_NET_NUMBER_OF_DESKTOPS     = XInternAtom(GDK_DISPLAY(), "_NET_NUMBER_OF_DESKTOPS", False);
    a_NET_CURRENT_DESKTOP        = XInternAtom(GDK_DISPLAY(), "_NET_CURRENT_DESKTOP", False);
    a_NET_DESKTOP_NAMES          = XInternAtom(GDK_DISPLAY(), "_NET_DESKTOP_NAMES", False);
    a_NET_DESKTOP_GEOMETRY       = XInternAtom(GDK_DISPLAY(), "_NET_DESKTOP_GEOMETRY", False);
    a_NET_ACTIVE_WINDOW          = XInternAtom(GDK_DISPLAY(), "_NET_ACTIVE_WINDOW", False);
    a_NET_SUPPORTED              = XInternAtom(GDK_DISPLAY(), "_NET_SUPPORTED", False);
    // Per-window EWMH atoms
    a_NET_WM_DESKTOP             = XInternAtom(GDK_DISPLAY(), "_NET_WM_DESKTOP", False);
    a_NET_WM_STATE               = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE", False);
    a_NET_WM_STATE_SKIP_TASKBAR  = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_SKIP_TASKBAR", False);
    a_NET_WM_STATE_SKIP_PAGER    = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_SKIP_PAGER", False);
    a_NET_WM_STATE_STICKY        = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_STICKY", False);
    a_NET_WM_STATE_HIDDEN        = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_HIDDEN", False);
    a_NET_WM_STATE_SHADED        = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_SHADED", False);
    a_NET_WM_STATE_ABOVE         = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_ABOVE", False);
    a_NET_WM_STATE_BELOW         = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_BELOW", False);
    // BUG: _NET_WM_STATE_SHADED is interned a second time here – redundant assignment
    a_NET_WM_STATE_SHADED        = XInternAtom(GDK_DISPLAY(), "_NET_WM_STATE_SHADED", False);
    // Window type atoms
    a_NET_WM_WINDOW_TYPE         = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE", False);

    a_NET_WM_WINDOW_TYPE_DESKTOP = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    a_NET_WM_WINDOW_TYPE_DOCK    = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_DOCK", False);
    a_NET_WM_WINDOW_TYPE_TOOLBAR = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    a_NET_WM_WINDOW_TYPE_MENU    = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_MENU", False);
    a_NET_WM_WINDOW_TYPE_UTILITY = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_UTILITY", False);
    a_NET_WM_WINDOW_TYPE_SPLASH  = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_SPLASH", False);
    a_NET_WM_WINDOW_TYPE_DIALOG  = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_DIALOG", False);
    a_NET_WM_WINDOW_TYPE_NORMAL  = XInternAtom(GDK_DISPLAY(), "_NET_WM_WINDOW_TYPE_NORMAL", False);
    // BUG: _NET_WM_DESKTOP is interned a second time here – redundant assignment
    a_NET_WM_DESKTOP             = XInternAtom(GDK_DISPLAY(), "_NET_WM_DESKTOP", False);
    // Window name atoms (prefer _NET_WM_NAME over WM_NAME for UTF-8 support)
    a_NET_WM_NAME                = XInternAtom(GDK_DISPLAY(), "_NET_WM_NAME", False);
    a_NET_WM_VISIBLE_NAME        = XInternAtom(GDK_DISPLAY(), "_NET_WM_VISIBLE_NAME", False);
    // Strut atoms – tell WMs to reserve screen space for the panel
    a_NET_WM_STRUT               = XInternAtom(GDK_DISPLAY(), "_NET_WM_STRUT", False);
    a_NET_WM_STRUT_PARTIAL       = XInternAtom(GDK_DISPLAY(), "_NET_WM_STRUT_PARTIAL", False);
    // Icon data atom
    a_NET_WM_ICON                = XInternAtom(GDK_DISPLAY(), "_NET_WM_ICON", False);
    // KDE system-tray extension atom
    a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR
                                 = XInternAtom(GDK_DISPLAY(), "_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR", False);

    RET();
}


/*
 * fb_init - One-time initialisation of the fbpanel utility layer.
 *
 * Must be called after gtk_init() but before any other fb_* functions.
 * Interns all X11 atoms and caches the default GTK icon theme.
 *
 * No parameters; no return value.
 */
void fb_init()
{
    resolve_atoms();
    // gtk_icon_theme_get_default() returns a shared singleton – do NOT unref it
    icon_theme = gtk_icon_theme_get_default();
}

/*
 * fb_free - Cleanup counterpart to fb_init().
 *
 * Currently a no-op: the icon_theme singleton is owned by GTK and must NOT
 * be unref'd here (GTK takes care of it at shutdown).
 */
void fb_free()
{
    // MUST NOT be ref'd or unref'd
    // g_object_unref(icon_theme);
}

/*
 * Xclimsg - Send an EWMH client message to the root window.
 *
 * Constructs a 32-bit XClientMessageEvent and broadcasts it to the root
 * window with SubstructureNotify|SubstructureRedirect masks, which is the
 * EWMH-standard way to ask the window manager to perform an action on behalf
 * of @win (e.g. switch desktop, change state).
 *
 * @win  : Target window XID (e.g. a client window or GDK_ROOT_WINDOW()).
 * @type : Message type atom (e.g. a_NET_CURRENT_DESKTOP).
 * @l0-l4: 32-bit data words packed into the event's data.l[] array.
 *
 * No return value.  X11 errors are caught by the global error handler.
 *
 * ISSUE: gdk_display (the global GDK Display*) is used directly here.
 *        The recommended accessor is gdk_x11_get_default_xdisplay().
 */
void
Xclimsg(Window win, long type, long l0, long l1, long l2, long l3, long l4)
{
    XClientMessageEvent xev;

    xev.type = ClientMessage;
    xev.window = win;
    xev.send_event = True;
    xev.display = gdk_display;   // raw GDK display handle (deprecated access pattern)
    xev.message_type = type;
    xev.format = 32;             // data words are 32-bit wide
    xev.data.l[0] = l0;
    xev.data.l[1] = l1;
    xev.data.l[2] = l2;
    xev.data.l[3] = l3;
    xev.data.l[4] = l4;
    // Send to root with propagate=False; WM intercepts via SubstructureRedirect
    XSendEvent(GDK_DISPLAY(), GDK_ROOT_WINDOW(), False,
          (SubstructureNotifyMask | SubstructureRedirectMask),
          (XEvent *) & xev);
}

/*
 * Xclimsgwm - Send a WM_PROTOCOLS client message directly to a window.
 *
 * Used for ICCCM-style WM protocol messages (e.g. WM_DELETE_WINDOW).
 * Unlike Xclimsg(), this sends to the window itself (not root), with
 * propagate=False and no event mask, per the ICCCM specification.
 *
 * @win  : Target window XID.
 * @type : Message type atom – should be a_WM_PROTOCOLS.
 * @arg  : The specific protocol atom (e.g. a_WM_DELETE_WINDOW).
 *
 * No return value.
 */
void
Xclimsgwm(Window win, Atom type, Atom arg)
{
    XClientMessageEvent xev;

    xev.type = ClientMessage;
    xev.window = win;
    xev.message_type = type;
    xev.format = 32;
    xev.data.l[0] = arg;                // the specific WM protocol atom
    xev.data.l[1] = GDK_CURRENT_TIME;   // timestamp (macro expands to 0 = CurrentTime)
    // Send directly to the window with no mask – the window must handle it
    XSendEvent(GDK_DISPLAY(), win, False, 0L, (XEvent *) &xev);
}


/*
 * get_utf8_property - Read a UTF8_STRING X11 property from a window.
 *
 * @win  : X11 Window XID to query.
 * @atom : Property atom to read (e.g. a_NET_WM_NAME).
 *
 * Returns: A newly-allocated NUL-terminated UTF-8 string, or NULL on failure
 *          (window doesn't have the property, wrong type, or X error).
 *
 * Memory: Caller owns the returned string and must free it with g_free().
 *         The raw X property buffer is freed with XFree() internally.
 *
 * ISSUE: The function signature declares return type as void* but actually
 *        always returns gchar*.  Callers must cast.
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
    // Request up to G_MAXLONG/4 bytes (the standard "read all" idiom for XGetWindowProperty)
    result = XGetWindowProperty (GDK_DISPLAY(), win, atom, 0, G_MAXLONG, False,
          a_UTF8_STRING, &type, &format, &nitems,
          &bytes_after, &tmp);
    if (result != Success)
        return NULL;
    if (tmp) {
        // Validate type (UTF8_STRING), format (8-bit bytes), and non-empty data
        if (type == a_UTF8_STRING && format == 8 && nitems != 0)
            retval = g_strndup ((gchar *)tmp, nitems);  // copy exactly nitems bytes
        XFree (tmp);  // always free the X server buffer
    }
    return retval;

}

/*
 * get_utf8_property_list - Read a multi-string UTF8_STRING X11 property.
 *
 * EWMH stores some properties (e.g. _NET_DESKTOP_NAMES) as a sequence of
 * NUL-separated UTF-8 strings in a single property value.  This function
 * splits that sequence into a NULL-terminated string array.
 *
 * @win   : X11 Window XID to query.
 * @atom  : Property atom to read.
 * @count : OUT parameter – set to the number of strings in the returned array.
 *
 * Returns: A newly-allocated NULL-terminated array of newly-allocated UTF-8
 *          strings, or NULL on failure.  *count is set to the number of valid
 *          entries (not counting the NULL terminator).
 *
 * Memory: Caller owns the returned array and every string in it.
 *         Free with g_strfreev() or by iterating and calling g_free() on each
 *         string followed by g_free() on the array pointer itself.
 *         The raw X property buffer is freed with XFree() internally.
 *
 * ISSUE: The memmove() fallback path for non-NUL-terminated data (lines
 *        296-301) modifies the XGetWindowProperty buffer in-place before
 *        XFree()-ing it.  This is technically undefined behaviour because the
 *        buffer was allocated by the X server / Xlib allocator and should be
 *        treated as read-only after retrieval.  The memmove writes into
 *        `tmp` indirectly via `val`, then calls XFree(tmp).  In practice
 *        most Xlib implementations use malloc internally so this works, but
 *        it is not guaranteed by the X11 specification.
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

        // First pass: count NUL separators to determine the number of strings
        for (i = 0; i < nitems; i++) {
            if (!val[i])
                (*count)++;
        }

        // Allocate array with room for *count strings plus one extra for the
        // unterminated-tail case, plus one NULL terminator
        retval = g_new0 (char*, *count + 2);

        // Second pass: duplicate each NUL-terminated substring
        for (i = 0, s = val; i < *count; i++, s = s +  strlen (s) + 1) {
            retval[i] = g_strdup(s);
        }

        // Handle the edge case where the last string is not NUL-terminated
        if (val[nitems-1]) {
            result = nitems - (s - val);  // bytes remaining for the last fragment
            DBG("val does not ends by 0, moving last %d bytes\n", result);
            // ISSUE: modifying X-server-allocated buffer (see function header note)
            memmove(s - 1, s, result);
            val[nitems-1] = 0;
            DBG("s=%s\n", s -1);
            retval[i] = g_strdup(s - 1);
            (*count)++;
        }
    }
    XFree (tmp);  // free the Xlib-allocated property buffer

    return retval;

}

/*
 * get_xaproperty - Generic X11 window property reader.
 *
 * Reads any window property into a newly-allocated buffer using
 * XGetWindowProperty with a large offset limit (0x7fffffff) to retrieve
 * the entire property in one round trip.
 *
 * @win    : X11 Window XID to query.
 * @prop   : Property atom to read.
 * @type   : Expected type atom (e.g. XA_CARDINAL, XA_ATOM, XA_WINDOW).
 *           XGetWindowProperty will accept AnyPropertyType if type is None.
 * @nitems : OUT parameter – if non-NULL, set to the number of items returned.
 *           Each "item" is format/8 bytes (e.g. 4 bytes for 32-bit CARDINAL).
 *
 * Returns: A pointer to the raw property data allocated by Xlib, or NULL on
 *          failure (X error, window destroyed, property not set, type mismatch).
 *
 * Memory: The returned buffer MUST be freed with XFree(), not g_free().
 *         Caller is responsible for this.
 *
 * ISSUE: No type validation is performed here; if the server returns data of
 *        a different type than requested, the data is still returned. Callers
 *        should validate type_ret if correctness matters (e.g. for security).
 */
void *
get_xaproperty (Window win, Atom prop, Atom type, int *nitems)
{
    Atom type_ret;
    int format_ret;
    unsigned long items_ret;
    unsigned long after_ret;
    unsigned char *prop_data;

    ENTER;
    prop_data = NULL;
    if (XGetWindowProperty (GDK_DISPLAY(), win, prop, 0, 0x7fffffff, False,
              type, &type_ret, &format_ret, &items_ret,
              &after_ret, &prop_data) != Success)
        RET(NULL);
    DBG("win=%x prop=%d type=%d rtype=%d rformat=%d nitems=%d\n", win, prop,
            type, type_ret, format_ret, items_ret);

    if (nitems)
        *nitems = items_ret;
    RET(prop_data);  // caller must XFree() this
}

/*
 * text_property_to_utf8 - Convert an XTextProperty to a UTF-8 string.
 *
 * Wraps gdk_text_property_to_utf8_list(), which handles conversion from
 * legacy ICCCM encodings (e.g. Latin-1, Compound Text) to UTF-8.
 *
 * @prop : Pointer to an XTextProperty (value, encoding, format, nitems).
 *
 * Returns: A newly-allocated UTF-8 string (the first element of the list),
 *          or NULL if conversion produced zero strings.
 *
 * Memory: Caller must free the returned string with g_free().
 *         The input XTextProperty.value buffer is NOT freed here; the caller
 *         is responsible for calling XFree(prop->value) after this returns.
 *
 * Note: Only the first string from the conversion list is returned; compound
 *       text with multiple values loses all but the first.
 */
static char*
text_property_to_utf8 (const XTextProperty *prop)
{
  char **list;
  int count;
  char *retval;

  ENTER;
  list = NULL;
  // Convert from X encoding to a list of UTF-8 strings; list is allocated by GDK
  count = gdk_text_property_to_utf8_list (gdk_x11_xatom_to_atom (prop->encoding),
                                          prop->format,
                                          prop->value,
                                          prop->nitems,
                                          &list);

  DBG("count=%d\n", count);
  if (count == 0)
    return NULL;

  retval = list[0];             // take ownership of first string
  list[0] = g_strdup ("");     // replace with empty string so g_strfreev() can free it
  g_strfreev (list);           // free the list structure and all remaining strings

  RET(retval);  // caller owns this string
}

/*
 * get_textproperty - Read an ICCCM XTextProperty from a window and convert to UTF-8.
 *
 * Used for legacy WM_NAME / WM_ICON_NAME properties that are stored in
 * ICCCM text encoding rather than UTF-8.
 *
 * @win  : X11 Window XID to query.
 * @atom : Property atom to read (e.g. a_WM_CLASS).
 *
 * Returns: A newly-allocated UTF-8 string, or NULL on failure.
 *
 * Memory: Caller must free the returned string with g_free().
 *         The XTextProperty.value buffer is freed with XFree() internally.
 *
 * ISSUE: The XFree() guard condition checks (text_prop.nitems > 0) but
 *        XGetTextProperty can succeed with nitems == 0 and still allocate
 *        a non-NULL value buffer.  This could cause a memory leak.
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
        retval = text_property_to_utf8 (&text_prop);
        // Only free value if there was actually data; see ISSUE above re nitems==0
        if (text_prop.nitems > 0)
            XFree (text_prop.value);
        RET(retval);

    }
    RET(NULL);
}


/*
 * get_net_number_of_desktops - Query the current desktop count from the WM.
 *
 * Reads _NET_NUMBER_OF_DESKTOPS as a CARDINAL from the root window.
 *
 * Returns: Number of virtual desktops, or 0 if the property is not set
 *          (e.g. no EWMH-compliant WM is running).
 *
 * Memory: Internally allocated buffer is freed with XFree() before returning.
 */
guint
get_net_number_of_desktops()
{
    guint desknum;
    guint32 *data;

    ENTER;
    data = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_NUMBER_OF_DESKTOPS,
          XA_CARDINAL, 0);  // passing 0 for nitems (don't need count)
    if (!data)
        RET(0);

    desknum = *data;
    XFree (data);  // release Xlib-allocated buffer
    RET(desknum);
}


/*
 * get_net_current_desktop - Query the currently active virtual desktop index.
 *
 * Reads _NET_CURRENT_DESKTOP as a CARDINAL from the root window.
 *
 * Returns: Zero-based desktop index, or 0 on failure (property not set).
 *
 * Memory: Internally allocated buffer is freed with XFree() before returning.
 */
guint
get_net_current_desktop ()
{
    guint desk;
    guint32 *data;

    ENTER;
    data = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, XA_CARDINAL, 0);
    if (!data)
        RET(0);

    desk = *data;
    XFree (data);  // release Xlib-allocated buffer
    RET(desk);
}

/*
 * get_net_wm_desktop - Query which virtual desktop a specific window is on.
 *
 * Reads _NET_WM_DESKTOP as a CARDINAL from the given window.  The special
 * value 0xFFFFFFFF means "sticky" (appears on all desktops).
 *
 * @win : X11 Window XID to query.
 *
 * Returns: Zero-based desktop index for the window, or 0 on failure.
 *          Note: 0 is also a valid desktop index, so callers cannot
 *          distinguish "desktop 0" from "property not set" without additional
 *          checks.
 *
 * Memory: Internally allocated buffer is freed with XFree() before returning.
 */
guint
get_net_wm_desktop(Window win)
{
    guint desk = 0;
    guint *data;

    ENTER;
    data = get_xaproperty (win, a_NET_WM_DESKTOP, XA_CARDINAL, 0);
    if (data) {
        desk = *data;
        XFree (data);  // release Xlib-allocated buffer
    } else
        DBG("can't get desktop num for win 0x%lx", win);
    RET(desk);
}

/*
 * get_net_wm_state - Populate a net_wm_state struct from a window's EWMH state.
 *
 * Reads the _NET_WM_STATE property (an array of Atom values) from @win and
 * sets the corresponding bitfield members in @nws.  Only the subset of atoms
 * known to fbpanel is decoded; unknown state atoms are silently ignored.
 *
 * @win : X11 Window XID to query.
 * @nws : OUT parameter – pointer to a net_wm_state struct to fill.
 *        The struct is zeroed via bzero() before being populated.
 *
 * No return value.  @nws is always valid (zeroed) after return.
 *
 * Memory: The Atom array returned by get_xaproperty() is freed with
 *         XFree() internally.
 *
 * ISSUE: get_net_wm_state does not decode _NET_WM_STATE_ABOVE or
 *        _NET_WM_STATE_BELOW even though those atoms and struct fields exist.
 */
void
get_net_wm_state(Window win, net_wm_state *nws)
{
    Atom *state;
    int num3;

    ENTER;
    bzero(nws, sizeof(*nws));  // zero all flags before reading
    if (!(state = get_xaproperty(win, a_NET_WM_STATE, XA_ATOM, &num3)))
        RET();  // property not set – return zeroed struct

    DBG( "%x: netwm state = { ", (unsigned int)win);
    // Iterate backwards through the atom array (avoids decrement-before-loop quirk)
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
            DBGE("... ");  // unrecognised state atom – ignored
        }
    }
    XFree(state);  // release Xlib-allocated atom array
    DBGE( "}\n");
    RET();
}




/*
 * get_net_wm_window_type - Populate a net_wm_window_type struct from a window's EWMH type.
 *
 * Reads the _NET_WM_WINDOW_TYPE property (an array of Atom values) from @win
 * and sets the corresponding bitfield members in @nwwt.  Per the EWMH spec,
 * the property may contain multiple type atoms in preference order; all
 * recognised ones are decoded.
 *
 * @win  : X11 Window XID to query.
 * @nwwt : OUT parameter – pointer to net_wm_window_type struct to fill.
 *         Zeroed with bzero() before being populated.
 *
 * No return value.  @nwwt is always valid (zeroed) after return.
 *
 * Memory: The Atom array returned by get_xaproperty() is freed with
 *         XFree() internally.
 */
void
get_net_wm_window_type(Window win, net_wm_window_type *nwwt)
{
    Atom *state;
    int num3;

    ENTER;
    bzero(nwwt, sizeof(*nwwt));  // zero all flags before reading
    if (!(state = get_xaproperty(win, a_NET_WM_WINDOW_TYPE, XA_ATOM, &num3)))
        RET();  // property not set – return zeroed struct

    DBG( "%x: netwm state = { ", (unsigned int)win);
    while (--num3 >= 0) {
        if (state[num3] == a_NET_WM_WINDOW_TYPE_DESKTOP) {
            DBG("NET_WM_WINDOW_TYPE_DESKTOP ");
            nwwt->desktop = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_DOCK) {
            DBG( "NET_WM_WINDOW_TYPE_DOCK ");
            nwwt->dock = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_TOOLBAR) {
            DBG( "NET_WM_WINDOW_TYPE_TOOLBAR ");
            nwwt->toolbar = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_MENU) {
            DBG( "NET_WM_WINDOW_TYPE_MENU ");
            nwwt->menu = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_UTILITY) {
            DBG( "NET_WM_WINDOW_TYPE_UTILITY ");
            nwwt->utility = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_SPLASH) {
            DBG( "NET_WM_WINDOW_TYPE_SPLASH ");
            nwwt->splash = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_DIALOG) {
            DBG( "NET_WM_WINDOW_TYPE_DIALOG ");
            nwwt->dialog = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_NORMAL) {
            DBG( "NET_WM_WINDOW_TYPE_NORMAL ");
            nwwt->normal = 1;
        } else {
            DBG( "... ");  // unrecognised type atom – ignored
        }
    }
    XFree(state);  // release Xlib-allocated atom array
    DBG( "}\n");
    RET();
}




/*
 * calculate_width - Compute the actual panel width and X origin for one axis.
 *
 * This is a static helper called by calculate_position().  It applies the
 * width type (percent / pixel), enforces the screen boundary, and then
 * positions the panel according to the alignment and margin settings.
 *
 * @scrw   : Total screen dimension along the axis being calculated (pixels).
 * @wtype  : Width type constant: WIDTH_PERCENT, WIDTH_PIXEL, or WIDTH_REQUEST.
 * @allign : Alignment constant: ALLIGN_LEFT, ALLIGN_RIGHT, or ALLIGN_CENTER.
 * @xmargin: Margin in pixels from the chosen edge (left/right/top/bottom).
 * @panw   : IN/OUT – On entry, the requested width/percent value; on exit,
 *           the computed width in pixels.
 * @x      : IN/OUT – On entry, the base coordinate (minx/miny from Xinerama);
 *           on exit, the final screen coordinate for the panel's origin.
 *
 * No return value.
 *
 * ISSUE: When wtype == WIDTH_PERCENT and alignment != CENTER, the margin is
 *        silently skipped (the commented-out MAX line and the bare ';').  This
 *        means xmargin has no effect for percent-width panels unless they are
 *        centered.  This is almost certainly unintentional.
 *
 * ISSUE: No check is made that (*panw) is positive after the percent
 *        calculation when the result rounds to zero (unlikely but possible
 *        for very small panels on very high-DPI screens).
 */
static void
calculate_width(int scrw, int wtype, int allign, int xmargin,
      int *panw, int *x)
{
    ENTER;
    DBG("scrw=%d\n", scrw);
    DBG("IN panw=%d\n", *panw);
    //scrw -= 2;
    if (wtype == WIDTH_PERCENT) {
        /* Clamp percent value to [1, 100] range */
        if (*panw > 100)
            *panw = 100;
        else if (*panw < 0)
            *panw = 1;
        // Convert percent to pixels; floating-point multiplication avoids
        // integer overflow for scrw values up to ~21M pixels
        *panw = ((gfloat) scrw * (gfloat) *panw) / 100.0;
    }
    // Ensure panel doesn't exceed the screen size
    if (*panw > scrw)
        *panw = scrw;

    if (allign != ALLIGN_CENTER) {
        // Sanity-check margin: if margin > screen, ignore it entirely
        if (xmargin > scrw) {
            ERR( "xmargin is bigger then edge size %d > %d. Ignoring xmargin\n",
                  xmargin, scrw);
            xmargin = 0;
        }
        if (wtype == WIDTH_PERCENT)
            //*panw = MAX(scrw - xmargin, *panw);
            ;  // BUG: margin is intentionally ignored for percent-width panels
        else
            // Clamp pixel-width panel so it fits within the margin
            *panw = MIN(scrw - xmargin, *panw);
    }
    DBG("OUT panw=%d\n", *panw);
    // Compute the x origin based on alignment
    if (allign == ALLIGN_LEFT)
        *x += xmargin;                            // left-aligned: shift right by margin
    else if (allign == ALLIGN_RIGHT) {
        *x += scrw - *panw - xmargin;             // right-aligned: from the right edge
        if (*x < 0)
            *x = 0;                               // prevent negative coordinate
    } else if (allign == ALLIGN_CENTER)
        *x += (scrw - *panw) / 2;                // centered: simple midpoint formula
    RET();
}


/*
 * calculate_position - Compute the panel's screen position (ax, ay, aw, ah).
 *
 * Determines where the panel should be placed on screen by combining:
 *   - The configured edge (top/bottom/left/right).
 *   - The configured alignment and margins.
 *   - Xinerama/multi-monitor geometry (if a specific head was requested).
 *
 * Results are written into the panel struct: np->ax, np->ay (origin),
 * np->aw, np->ah (dimensions in pixels).  These are then used by
 * gtk_window_move() and gtk_window_resize() in panel_start_gui().
 *
 * @np : Pointer to the panel struct.  Fields read: edge, widthtype, width,
 *       height, heighttype, allign, xmargin, ymargin, xineramaHead.
 *       Fields written: ax, ay, aw, ah, screenRect.
 *
 * No return value.
 *
 * ISSUE: If np->xineramaHead is a valid index but the Xinerama monitor list
 *        has changed since startup (monitor hotplug), gdk_screen_get_monitor_geometry
 *        could return stale data.  No refresh mechanism exists.
 *
 * ISSUE: aw=0 / ah=0 are clamped to 1 at the end to avoid zero-size windows.
 *        However, gdk/X11 may still reject a 1x1 window in some configurations.
 */
void
calculate_position(panel *np)
{
    int positionSet = 0;          // flag: 1 if Xinerama geometry was successfully determined
    int sswidth, ssheight, minx, miny;

    ENTER;

    /* If a Xinerama head was specified on the command line, then
     * calculate the location based on that.  Otherwise, just use the
     * screen dimensions. */
    if(np->xineramaHead != FBPANEL_INVALID_XINERAMA_HEAD) {
      GdkScreen *screen = gdk_screen_get_default();
      int nDisplay = gdk_screen_get_n_monitors(screen);
      GdkRectangle rect;

      // Validate that the requested head index is within the monitor count
      if(np->xineramaHead < nDisplay) {
        gdk_screen_get_monitor_geometry(screen, np->xineramaHead, &rect);
        minx = rect.x;
        miny = rect.y;
        sswidth = rect.width;
        ssheight = rect.height;
        positionSet = 1;  // successfully obtained Xinerama geometry
      }
      // ISSUE: If xineramaHead >= nDisplay, positionSet remains 0 and we fall
      //        through to the full-screen defaults silently (no warning logged).
    }

    if (!positionSet)  {
        // No valid Xinerama head – use the full virtual screen dimensions
        minx = miny = 0;
        sswidth  = gdk_screen_width();
        ssheight = gdk_screen_height();

    }

    // Record the screen rectangle for this panel (used by menu_pos)
    np->screenRect.x = minx;
    np->screenRect.y = miny;
    np->screenRect.width = sswidth;
    np->screenRect.height = ssheight;

    if (np->edge == EDGE_TOP || np->edge == EDGE_BOTTOM) {
        /* Horizontal panel: width runs along the X axis, height is fixed */
        np->aw = np->width;                // start with the configured width value
        np->ax = minx;                     // start from the left edge of the screen
        calculate_width(sswidth, np->widthtype, np->allign, np->xmargin,
              &np->aw, &np->ax);           // apply type, alignment, margin
        np->ah = np->height;
        np->ah = MIN(PANEL_HEIGHT_MAX, np->ah);  // clamp to max allowed height
        np->ah = MAX(PANEL_HEIGHT_MIN, np->ah);  // clamp to min allowed height
        if (np->edge == EDGE_TOP)
            np->ay = np->ymargin;          // top-edge panel: offset from top
        else
            np->ay = ssheight - np->ah - np->ymargin;  // bottom-edge: offset from bottom

    } else {
        /* Vertical panel: "width" config applies to the vertical (Y) dimension */
        np->ah = np->width;                // width config drives the panel's height
        np->ay = miny;                     // start from the top of the screen
        calculate_width(ssheight, np->widthtype, np->allign, np->xmargin,
              &np->ah, &np->ay);           // apply type, alignment, margin to the Y axis
        np->aw = np->height;               // height config drives the panel's pixel width
        np->aw = MIN(PANEL_HEIGHT_MAX, np->aw);
        np->aw = MAX(PANEL_HEIGHT_MIN, np->aw);
        if (np->edge == EDGE_LEFT)
            np->ax = np->ymargin;          // left-edge panel: offset from left
        else
            np->ax = sswidth - np->aw - np->ymargin;  // right-edge: offset from right
    }
    // Prevent zero-size windows which confuse X11
    if (!np->aw)
        np->aw = 1;
    if (!np->ah)
        np->ah = 1;

    /*
    if (!np->visible) {
        DBG("pushing of screen dx=%d dy=%d\n", np->ah_dx, np->ah_dy);
        np->ax += np->ah_dx;
        np->ay += np->ah_dy;
    }
    */
    DBG("x=%d y=%d w=%d h=%d\n", np->ax, np->ay, np->aw, np->ah);
    RET();
}



/*
 * expand_tilda - Expand a leading '~' in a file path to the $HOME directory.
 *
 * @file : Input file path string.  If NULL, NULL is returned.
 *         If the first character is '~', $HOME is prepended to the rest.
 *         Otherwise the string is duplicated unchanged.
 *
 * Returns: A newly-allocated string with '~' expanded, or a duplicate of
 *          @file if no expansion is needed.  Returns NULL if @file is NULL.
 *
 * Memory: Caller must free the returned string with g_free().
 *
 * ISSUE: getenv("HOME") can return NULL (e.g. in minimal environments or
 *        sandboxed processes).  If it does, g_strdup_printf() will receive a
 *        NULL format argument and likely crash or produce "(null)" depending
 *        on the platform's printf implementation.
 */
gchar *
expand_tilda(gchar *file)
{
    ENTER;
    if (!file)
        RET(NULL);
    RET((file[0] == '~') ?
        g_strdup_printf("%s%s", getenv("HOME"), file+1)  // expand ~ to $HOME
        : g_strdup(file));                                // no ~ – just duplicate

}


/*
 * get_button_spacing - Measure the minimum size of a named GtkButton.
 *
 * Creates a temporary GtkButton with the given CSS name, optionally adds it
 * to @parent (so the theme is applied), calls gtk_widget_size_request() to
 * measure it, then immediately destroys it.  The result tells callers how
 * much space a button consumes beyond its icon/label content.
 *
 * @req    : OUT parameter – filled with the button's size requisition.
 * @parent : Optional container to temporarily host the button (may be NULL).
 *           If non-NULL, the button is added to the container; adding to the
 *           container passes widget ownership to it, and gtk_widget_destroy()
 *           removes it from the container and finalises it.
 * @name   : Widget name to set via gtk_widget_set_name() for theme lookups.
 *
 * No return value.
 *
 * ISSUE: GTK_WIDGET_UNSET_FLAGS() is a deprecated API in GTK2.2+.  The
 *        correct replacement is gtk_widget_set_can_focus(b, FALSE) and
 *        gtk_widget_set_can_default(b, FALSE).
 *
 * ISSUE: gtk_widget_size_request() is also deprecated in later GTK2 builds;
 *        gtk_widget_get_preferred_size() is the GTK3 equivalent.
 *
 * ISSUE: If @parent is provided and the button is not shown, size_request
 *        may return zeros or inaccurate values on some GTK themes.
 */
void
get_button_spacing(GtkRequisition *req, GtkContainer *parent, gchar *name)
{
    GtkWidget *b;
    //gint focus_width;
    //gint focus_pad;

    ENTER;
    b = gtk_button_new();
    gtk_widget_set_name(GTK_WIDGET(b), name);   // apply CSS name for theme matching
    GTK_WIDGET_UNSET_FLAGS (b, GTK_CAN_FOCUS);  // deprecated – avoids focus ring space
    GTK_WIDGET_UNSET_FLAGS (b, GTK_CAN_DEFAULT); // deprecated – avoids default-button border
    gtk_container_set_border_width (GTK_CONTAINER (b), 0);

    if (parent)
        gtk_container_add(parent, b);  // transfers ownership to parent

    gtk_widget_show(b);
    gtk_widget_size_request(b, req);  // fills *req with width, height in pixels

    gtk_widget_destroy(b);  // removes from parent (if any) and finalises
    RET();
}


/*
 * gcolor2rgb24 - Convert a GdkColor to a packed 24-bit RGB integer.
 *
 * GdkColor stores each channel in 16-bit range [0, 65535].  This function
 * scales each channel to [0, 255] and packs them into a single guint32
 * with the layout 0x00RRGGBB.
 *
 * @color : Pointer to the GdkColor to convert.
 *
 * Returns: Packed 24-bit colour value (bits 31-24 are always zero).
 *
 * ISSUE: The scaling formula (channel * 0xFF / 0xFFFF) is integer division,
 *        which truncates rather than rounds.  For most values this is fine,
 *        but a 16-bit value of 0xFFFF (pure white) correctly maps to 0xFF,
 *        and 0x0000 maps to 0x00 – so the edge cases are correct.
 */
guint32
gcolor2rgb24(GdkColor *color)
{
    guint32 i;

    ENTER;
#ifdef DEBUGPRN
    {
        guint16 r, g, b;

        r = color->red * 0xFF / 0xFFFF;
        g = color->green * 0xFF / 0xFFFF;
        b = color->blue * 0xFF / 0xFFFF;
        DBG("%x %x %x ==> %x %x %x\n", color->red, color->green, color->blue,
            r, g, b);
    }
#endif
    // Build 0x00RRGGBB by shifting red into position 16, green into 8, blue into 0
    i = (color->red * 0xFF / 0xFFFF) & 0xFF;    // red channel (8 bits)
    i <<= 8;
    i |= (color->green * 0xFF / 0xFFFF) & 0xFF; // green channel (8 bits)
    i <<= 8;
    i |= (color->blue * 0xFF / 0xFFFF) & 0xFF;  // blue channel (8 bits)
    DBG("i=%x\n", i);
    RET(i);
}

/*
 * gdk_color_to_RRGGBB - Convert a GdkColor to a CSS "#RRGGBB" hex string.
 *
 * Uses the high byte of each 16-bit channel (red>>8, green>>8, blue>>8) to
 * produce an 8-bit-per-channel hex representation.
 *
 * @color : Pointer to the GdkColor to convert.
 *
 * Returns: Pointer to a static internal buffer containing the "#RRGGBB"
 *          string.  This buffer is overwritten on every call.
 *
 * ISSUE: The returned pointer is a static buffer – this function is NOT
 *        thread-safe and NOT safe to call twice and compare both results.
 *        Callers that need a persistent string must copy with g_strdup().
 *        gconf_edit_color_cb() passes the return value directly to
 *        xconf_set_value(), which calls g_strdup() on it, so it is safe
 *        there – but future callers must be careful.
 */
gchar *
gdk_color_to_RRGGBB(GdkColor *color)
{
    static gchar str[10]; // buffer: '#' + 6 hex digits + '\0' = 8 bytes (10 for safety)
    g_snprintf(str, sizeof(str), "#%02x%02x%02x",
        color->red >> 8, color->green >> 8, color->blue >> 8);
    return str;  // WARNING: static buffer – see ISSUE above
}

/*
 * menu_pos - GtkMenuPositionFunc callback to position the panel's context menu.
 *
 * This function is passed as the position function to gtk_menu_popup().  It
 * computes a position for the menu that keeps it:
 *   a) Near the widget that was clicked (or the mouse cursor if widget==NULL).
 *   b) Entirely within the valid screen area (the screen minus the panel bar
 *      itself, so the menu doesn't overlap the panel).
 *
 * @menu    : The GtkMenu being positioned.
 * @x       : OUT parameter – the computed x coordinate for the menu.
 * @y       : OUT parameter – the computed y coordinate for the menu.
 * @push_in : OUT parameter – always set to TRUE (GTK should push menu on-screen
 *            if it overflows, but we do our own clamping here).
 * @widget  : The widget relative to which the menu should appear, or NULL to
 *            use the current mouse pointer position.
 *
 * ISSUE: Accesses the global the_panel directly; will break with multi-panel
 *        support.
 *
 * ISSUE: When @widget is NULL and the pointer coordinates are used, the 20px
 *        offsets are hardcoded and may place the menu off-screen on very
 *        small monitors.
 *
 * ISSUE: Uses deprecated GTK2 direct struct access (widget->window,
 *        widget->allocation, GTK_WIDGET(menu)->requisition) instead of
 *        accessor functions.
 */
void
menu_pos(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, GtkWidget *widget)
{
    GdkRectangle menuRect;
    // Copy the panel's screen rectangle as the starting "valid area"
    GdkRectangle validRect = the_panel->screenRect;

    ENTER;

    /* Shrink validRect to exclude the panel bar itself, so the menu won't
     * be placed on top of the panel. */
    if(the_panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
      validRect.height -= the_panel->ah;  // remove panel height from available area
      if(the_panel->edge == EDGE_TOP) {
        validRect.y += the_panel->ah;     // top panel: shift valid area downward
      }
    } else {
      validRect.width -= the_panel->aw;   // remove panel width from available area
      if(the_panel->edge == EDGE_LEFT) {
        validRect.x += the_panel->aw;     // left panel: shift valid area rightward
      }
    }

    /* Calculate a requested location based on the widget/mouse location,
     * relative to the root window. */
    if (widget) {
        // Get the widget's absolute position and add its allocation offset
        gdk_window_get_origin(widget->window, &menuRect.x, &menuRect.y);
        menuRect.x += widget->allocation.x;  // add intra-window offset
        menuRect.y += widget->allocation.y;
    } else {
        // No widget: use the current mouse cursor position
        gdk_display_get_pointer(gdk_display_get_default(), NULL,
                                &menuRect.x, &menuRect.y, NULL);
        menuRect.x -= 20;  // offset so menu appears near but not under the cursor
        menuRect.y -= 20;
    }

    // Get the menu's required dimensions (before it is shown)
    menuRect.width = GTK_WIDGET(menu)->requisition.width;   // deprecated direct access
    menuRect.height = GTK_WIDGET(menu)->requisition.height; // deprecated direct access

    /* Clamp menuRect to fit within validRect on all four sides */
    if(menuRect.x + menuRect.width > validRect.x + validRect.width) {
      menuRect.x = validRect.x + validRect.width - menuRect.width;
    }
    if(menuRect.x < validRect.x) {
      menuRect.x = validRect.x;
    }
    if(menuRect.y + menuRect.height > validRect.y + validRect.height) {
      menuRect.y = validRect.y + validRect.height - menuRect.height;
    }
    if(menuRect.y < validRect.y) {
      menuRect.y = validRect.y;
    }

    *x = menuRect.x;
    *y = menuRect.y;

    DBG("w-h %d %d\n", menuRect.width, menuRect.height);
    *push_in = TRUE;  // tell GTK to also do its own overflow correction
    RET();
}


/*
 * indent - Return a static indentation string for a given nesting level.
 *
 * Used by xconf_prn() to format config file output.  Each level is 4 spaces.
 * Levels above the table size are clamped to the maximum entry.
 *
 * @level : Nesting depth (0-4).
 *
 * Returns: Pointer to a static string of (level * 4) spaces.
 *          Caller must NOT free or modify the returned string.
 *
 * ISSUE: The bounds check uses sizeof(space) (which gives the size of the
 *        pointer array in bytes, not the number of entries).  On a 64-bit
 *        system, sizeof(space) == 5 * 8 = 40, so any level > 40 is clamped
 *        to 40, which then causes an out-of-bounds array access (space[40]
 *        is undefined behaviour).  The check should be
 *        sizeof(space)/sizeof(space[0]) to get the element count (5).
 */
gchar *
indent(int level)
{
    static gchar *space[] = {
        "",
        "    ",
        "        ",
        "            ",
        "                ",
    };

    // BUG: sizeof(space) returns byte count of pointer array, not element count.
    // On a 64-bit system this is 40, making the clamping nearly useless and
    // allowing out-of-bounds access for levels 5..40.
    if (level > sizeof(space))
        level = sizeof(space);
    RET(space[level]);
}




/**********************************************************************
 * FB Pixbuf                                                          *
 **********************************************************************/

/* Maximum dimension (in pixels) used when loading icons from the theme.
 * Icons larger than this are scaled down. */
#define MAX_SIZE 192

/*
 * fb_pixbuf_new - Load a GdkPixbuf from an icon name or file, with fallback.
 *
 * Tries the following sources in order, stopping at the first success:
 *   1. GTK icon theme lookup by @iname.
 *   2. File load by @fname at the requested size.
 *   3. The "gtk-missing-image" icon from the theme (if @use_fallback is TRUE).
 *
 * @iname        : Icon theme name (e.g. "applications-internet"), or NULL.
 * @fname        : Absolute file path to an image file, or NULL.
 * @width        : Desired width in pixels (used for file load and size clamping).
 * @height       : Desired height in pixels.
 * @use_fallback : If TRUE and neither @iname nor @fname succeeded, load the
 *                 "gtk-missing-image" placeholder icon.
 *
 * Returns: A new GdkPixbuf reference, or NULL if all sources failed.
 *          The result is always no larger than MAX_SIZE (192) pixels in either
 *          dimension when loaded from the icon theme.
 *
 * Memory: Caller owns the returned GdkPixbuf and must unref with
 *         g_object_unref() when done.
 *
 * Ref-count: gtk_icon_theme_load_icon() and gdk_pixbuf_new_from_file_at_size()
 *            both return a pixbuf with an initial ref-count of 1.
 */
GdkPixbuf *
fb_pixbuf_new(gchar *iname, gchar *fname, int width, int height,
        gboolean use_fallback)
{
    GdkPixbuf *pb = NULL;
    int size;

    ENTER;
    // Clamp the requested size to MAX_SIZE (icon theme uses a single dimension)
    size = MIN(192, MAX(width, height));
    // Try loading from the current GTK icon theme by name
    if (iname && !pb)
        pb = gtk_icon_theme_load_icon(icon_theme, iname, size,
            GTK_ICON_LOOKUP_FORCE_SIZE, NULL);  // NULL: ignore GError
    // Fall back to loading from a file path
    if (fname && !pb)
        pb = gdk_pixbuf_new_from_file_at_size(fname, width, height, NULL);
    // Final fallback: standard "missing image" indicator
    if (use_fallback && !pb)
        pb = gtk_icon_theme_load_icon(icon_theme, "gtk-missing-image", size,
            GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    RET(pb);  // may be NULL if all sources failed and use_fallback is FALSE
}

/*
 * fb_pixbuf_make_back_image - Create a brightened (hover) version of a pixbuf.
 *
 * Produces a copy of @front with each non-transparent pixel brightened by
 * adding @hicolor's RGB components (clamped at 255).  The result is used as
 * the "mouse-over" image for an fb_button.
 *
 * @front   : Source pixbuf (read-only; not modified).
 * @hicolor : 24-bit highlight colour in 0x00RRGGBB format.  Each byte is
 *            added to the corresponding channel of each source pixel.
 *
 * Returns: A new GdkPixbuf reference with the highlight applied, or:
 *          - @front itself (with a ref added) if gdk_pixbuf_add_alpha() fails.
 *          - @front itself (with a ref added) if @front is NULL.
 *
 * Memory: Caller owns the returned GdkPixbuf and must unref it.
 *
 * Ref-count: gdk_pixbuf_add_alpha() returns a new pixbuf (ref=1).
 *            The fallback path calls g_object_ref(front) to give the caller
 *            a reference to return and unref symmetrically.
 *
 * ISSUE: If @front has no alpha channel, gdk_pixbuf_add_alpha() creates a
 *        new RGBA pixbuf – this is expected and correct.
 *
 * ISSUE: The pixel iteration does not validate that the pixbuf's colorspace
 *        is GDK_COLORSPACE_RGB.  Other colorspaces (unlikely but possible)
 *        would cause incorrect results.
 */
static GdkPixbuf *
fb_pixbuf_make_back_image(GdkPixbuf *front, gulong hicolor)
{
    GdkPixbuf *back;
    guchar *src, *up, extra[3];
    int i;

    ENTER;
    if(!front)
    {
        RET(front);  // NULL in, NULL out (no ref added)
    }
    // Create a copy with an alpha channel; FALSE = don't make any pixel transparent
    back = gdk_pixbuf_add_alpha(front, FALSE, 0, 0, 0);
    if (!back) {
        // Fallback: ref front and return it unchanged
        g_object_ref(G_OBJECT(front));
        RET(front);
    }
    src = gdk_pixbuf_get_pixels (back);  // raw RGBA pixel data (4 bytes/pixel)
    // Unpack hicolor 0xRRGGBB into extra[0]=R, extra[1]=G, extra[2]=B
    for (i = 2; i >= 0; i--, hicolor >>= 8)
        extra[i] = hicolor & 0xFF;
    // Pointer to one-past-end of pixel buffer for the loop guard
    for (up = src + gdk_pixbuf_get_height(back) * gdk_pixbuf_get_rowstride (back);
            src < up; src+=4) {  // advance by 4 bytes (RGBA) per pixel
        if (src[3] == 0)
            continue;  // fully transparent pixel – skip (don't brighten transparent areas)
        // Add the highlight colour to each RGB channel, clamping at 255
        for (i = 0; i < 3; i++) {
            if (src[i] + extra[i] >= 255)
                src[i] = 255;
            else
                src[i] += extra[i];
        }
    }
    RET(back);  // caller must g_object_unref(back)
}

/* Number of pixels by which the pressed icon is inset on each side.
 * The pressed image is scaled down by 2*PRESS_GAP and centered. */
#define PRESS_GAP 2

/*
 * fb_pixbuf_make_press_image - Create a "pressed" (slightly smaller) version of a pixbuf.
 *
 * Scales @front down by 2*PRESS_GAP pixels in each dimension and centres the
 * result on a transparent copy of the original-size canvas.  This gives a
 * visual "inward push" effect when a button is clicked.
 *
 * @front : Source pixbuf (read-only; not modified).
 *
 * Returns: A new GdkPixbuf with the press effect, or @front (with a ref added)
 *          if any intermediate allocation fails.  Returns @front unchanged if
 *          @front is NULL.
 *
 * Memory: Caller owns the returned GdkPixbuf and must unref it.
 *
 * Ref-count: gdk_pixbuf_copy() and gdk_pixbuf_scale_simple() both return new
 *            pixbufs (ref=1).  Intermediate pixbufs are unref'd internally.
 *            The fallback path calls g_object_ref(front) to give the caller
 *            a reference to unref symmetrically.
 *
 * ISSUE: If gdk_pixbuf_scale_simple returns NULL (memory failure), the code
 *        correctly falls through to the error-cleanup block.
 *
 * ISSUE: If @front is smaller than 2*PRESS_GAP pixels in either dimension,
 *        w or h will be zero or negative.  gdk_pixbuf_scale_simple() with
 *        zero dimensions is undefined; this could crash.
 */
static GdkPixbuf *
fb_pixbuf_make_press_image(GdkPixbuf *front)
{
    GdkPixbuf *press, *tmp;
    int w, h;

    ENTER;
    if(!front)
    {
        RET(front);  // NULL in, NULL out (no ref added)
    }
    // Compute the target dimensions: inset by PRESS_GAP on all sides
    w = gdk_pixbuf_get_width(front) - 2 * PRESS_GAP;
    h = gdk_pixbuf_get_height(front) - 2 * PRESS_GAP;
    press = gdk_pixbuf_copy(front);                          // full-size canvas copy
    tmp = gdk_pixbuf_scale_simple(front, w, h, GDK_INTERP_HYPER);  // scaled-down version
    if (press && tmp) {
        gdk_pixbuf_fill(press, 0);       // clear canvas to fully transparent black
        gdk_pixbuf_copy_area(tmp,
                0, 0,          // src_x, src_y – copy from origin of scaled image
                w, h,          // width, height to copy
                press,         // dest_pixbuf – the full-size transparent canvas
                PRESS_GAP, PRESS_GAP);   // dst_x, dst_y – paste with inset offset
        g_object_unref(G_OBJECT(tmp));   // done with the scaled image
        RET(press);  // caller owns press
    }
    // Allocation failure cleanup
    if (press)
        g_object_unref(G_OBJECT(press));
    if (tmp)
        g_object_unref(G_OBJECT(tmp));

    // Fallback: return front with a new reference
    g_object_ref(G_OBJECT(front));
    RET(front);
}

/**********************************************************************
 * FB Image                                                           *
 *
 * fb_image is a GtkImage subtype (implemented via g_object_set_data)
 * that:
 *   - Loads its pixbuf via fb_pixbuf_new() at creation time.
 *   - Automatically reloads its pixbuf when the GTK icon theme changes
 *     (via the "changed" signal on the GtkIconTheme singleton).
 *   - Stores up to PIXBBUF_NUM pixbuf variants (normal, hover, pressed).
 *   - Frees all resources when the widget is destroyed ("destroy" signal).
 *
 * Internal state is stored in an fb_image_conf_t struct attached to the
 * widget via g_object_set_data(image, "conf", conf).
 **********************************************************************/

/* Number of pixbuf variants stored per fb_image:
 *   pix[0] = normal
 *   pix[1] = hover (brightened)
 *   pix[2] = pressed (scaled down) */
#define PIXBBUF_NUM 3

/*
 * fb_image_conf_t - Private state for an fb_image widget.
 *
 * Lifetime: allocated in fb_image_new(), freed in fb_image_free().
 * Attached to the GtkImage widget via g_object_set_data(image, "conf", conf).
 */
typedef struct {
    gchar *iname;          /* Icon theme name (owned; freed in fb_image_free) */
    gchar *fname;          /* File path (owned; freed in fb_image_free) */
    int width, height;     /* Requested icon dimensions in pixels */
    gulong itc_id;         /* Signal handler ID for icon-theme "changed" signal;
                            * disconnected in fb_image_free() */
    gulong hicolor;        /* 24-bit highlight colour (0x00RRGGBB) for pix[1] */
    int i;                 /* Index of the currently displayed pixbuf (0/1/2) */
    GdkPixbuf *pix[PIXBBUF_NUM]; /* Pixbuf variants; each may be NULL; owned by this struct */
} fb_image_conf_t;

static void fb_image_free(GObject *image);
static void fb_image_icon_theme_changed(GtkIconTheme *icon_theme,
        GtkWidget *image);

/*
 * fb_image_new - Create a GtkImage widget with automatic icon-theme tracking.
 *
 * Creates a standard GtkImage and attaches an fb_image_conf_t to it.
 * The image automatically updates when the GTK icon theme changes.
 * On destruction ("destroy" signal), fb_image_free() cleans up all resources.
 *
 * @iname  : Icon theme name (e.g. "gtk-quit"), or NULL.
 * @fname  : Absolute file path to an image, or NULL.
 * @width  : Desired icon width in pixels.
 * @height : Desired icon height in pixels.
 *
 * Returns: A GtkWidget* (GtkImage) with ref-count 1, already shown
 *          (gtk_widget_show() called).  The caller should add it to a
 *          container (which takes ownership) or manage the ref explicitly.
 *
 * Memory: The returned widget is shown but not yet in any container.
 *         iname and fname are duplicated; original strings are not consumed.
 *         pix[0] is loaded with use_fallback=TRUE (always non-NULL if the
 *         "gtk-missing-image" icon exists in the theme).
 *
 * Ref-count: The "changed" signal connects with g_signal_connect_after().
 *            The signal ID is stored in conf->itc_id for later disconnection.
 *            The widget's "destroy" signal connects fb_image_free() to clean up.
 *
 * ISSUE: Only pix[0] is populated here.  pix[1] (hover) and pix[2] (press)
 *        are created later in fb_button_new().  An fb_image that is not
 *        wrapped in an fb_button will never have hover/press variants.
 */
GtkWidget *
fb_image_new(gchar *iname, gchar *fname, int width, int height)
{
    GtkWidget *image;
    fb_image_conf_t *conf;

    image = gtk_image_new();
    conf = g_new0(fb_image_conf_t, 1);  // g_new0 calls g_error/exit on OOM
    // Attach conf to the widget; the widget does NOT own conf (we must free it ourselves)
    g_object_set_data(G_OBJECT(image), "conf", conf);
    // Connect to icon-theme "changed" to reload icons when the theme changes
    // g_signal_connect_after ensures our handler runs after GTK's own cleanup
    conf->itc_id = g_signal_connect_after (G_OBJECT(icon_theme),
            "changed", (GCallback) fb_image_icon_theme_changed, image);
    // Connect to the widget's own "destroy" so we can free conf when the widget dies
    g_signal_connect (G_OBJECT(image),
            "destroy", (GCallback) fb_image_free, NULL);
    conf->iname = g_strdup(iname);  // own a copy of the icon name
    conf->fname = g_strdup(fname);  // own a copy of the file path
    conf->width = width;
    conf->height = height;
    // Load the normal (non-hover) pixbuf immediately; use fallback if loading fails
    conf->pix[0] = fb_pixbuf_new(iname, fname, width, height, TRUE);
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), conf->pix[0]);
    gtk_widget_show(image);
    RET(image);
}


/*
 * fb_image_free - Destroy callback: release all resources owned by an fb_image.
 *
 * Connected to the GtkImage's "destroy" signal.  Called automatically by GTK
 * when the widget is being finalised (after gtk_widget_destroy()).
 *
 * @image : The GObject* being destroyed (is actually a GtkImage*).
 *
 * No return value.
 *
 * Memory freed:
 *   - The "changed" signal connection on the icon_theme singleton.
 *   - conf->iname and conf->fname (g_strdup'd copies).
 *   - Each non-NULL pixbuf in conf->pix[] (via g_object_unref).
 *   - The conf struct itself (via g_free).
 *
 * ISSUE: The "conf" data is retrieved from the GObject but is NOT cleared
 *        (g_object_set_data is not called with NULL).  This is safe because
 *        the widget is being destroyed, but is a minor style issue.
 *
 * Ref-count: Each g_object_unref(pix[i]) decrements the pixbuf's ref-count.
 *            If the pixbuf was shared (ref-count > 1), this does not free it.
 */
static void
fb_image_free(GObject *image)
{
    fb_image_conf_t *conf;
    int i;

    ENTER;
    conf = g_object_get_data(image, "conf");
    // Disconnect the icon-theme "changed" signal to prevent use-after-free
    g_signal_handler_disconnect(G_OBJECT(icon_theme), conf->itc_id);
    g_free(conf->iname);  // free the duplicated icon name
    g_free(conf->fname);  // free the duplicated file path
    // Unref all pixbuf variants
    for (i = 0; i < PIXBBUF_NUM; i++)
        if (conf->pix[i])
            g_object_unref(G_OBJECT(conf->pix[i]));
    g_free(conf);  // free the private state struct
    RET();
}

/*
 * fb_image_icon_theme_changed - Reload pixbufs when the GTK icon theme changes.
 *
 * Connected to GtkIconTheme's "changed" signal.  Discards all cached pixbufs
 * and reloads pix[0] from the (possibly new) icon theme.  Also rebuilds the
 * hover (pix[1]) and press (pix[2]) variants if hicolor is set.
 *
 * @icon_theme : The GtkIconTheme that emitted "changed" (shadows the global).
 * @image      : The GtkImage widget whose pixbufs should be refreshed.
 *
 * No return value.
 *
 * Ref-count: Each existing pixbuf is unref'd (destroying it if conf is the
 *            sole owner) and the slot is set to NULL before reloading.
 *            New pixbufs are loaded with ref=1 by the factory functions.
 *
 * ISSUE: fb_pixbuf_make_press_image() is called with conf->pix[1] (the hover
 *        image) as input, not conf->pix[0] (the normal image).  This is
 *        consistent with fb_button_new(), but means the press image is a
 *        brightened-then-shrunken image, not a plain shrunk image.  This
 *        matches the visual intent but is subtle.
 *
 * ISSUE: If conf->hicolor == 0, fb_pixbuf_make_back_image() will add 0 to
 *        every pixel channel, producing an identical copy of pix[0] wasting
 *        memory.  A check for hicolor == 0 would skip creating pix[1]/pix[2].
 */
static void
fb_image_icon_theme_changed(GtkIconTheme *icon_theme, GtkWidget *image)
{
    fb_image_conf_t *conf;
    int i;

    ENTER;
    conf = g_object_get_data(G_OBJECT(image), "conf");
    DBG("%s / %s\n", conf->iname, conf->fname);
    // Unref and clear all cached pixbufs so they will be reloaded
    for (i = 0; i < PIXBBUF_NUM; i++)
        if (conf->pix[i]) {
            g_object_unref(G_OBJECT(conf->pix[i]));
	    conf->pix[i] = NULL;
	}
    // Reload normal pixbuf from the updated icon theme
    conf->pix[0] = fb_pixbuf_new(conf->iname, conf->fname,
            conf->width, conf->height, TRUE);
    // Rebuild the hover pixbuf from the fresh normal pixbuf
    conf->pix[1] = fb_pixbuf_make_back_image(conf->pix[0], conf->hicolor);
    // Rebuild the press pixbuf from the hover pixbuf (same convention as fb_button_new)
    conf->pix[2] = fb_pixbuf_make_press_image(conf->pix[1]);
    // Display the normal (un-hovered) variant after theme reload
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), conf->pix[0]);
    RET();
}


/**********************************************************************
 * FB Button                                                          *
 *
 * fb_button is a composite widget: a GtkBgBox (pseudo-transparent
 * container) containing an fb_image.  The bgbox captures mouse events
 * and delegates to event handlers that swap pixbuf variants on the
 * fb_image to produce hover and press animations.
 *
 * Signal flow:
 *   bgbox "enter-notify-event" → fb_button_cross(image, event)
 *   bgbox "leave-notify-event" → fb_button_cross(image, event)
 *   bgbox "button-press-event" → fb_button_pressed(image, event)
 *   bgbox "button-release-event" → fb_button_pressed(image, event)
 *
 * Widget tree:
 *   GtkBgBox (b)
 *     └─ GtkImage (image, managed as fb_image)
 *
 * Ownership: fb_button_new() returns the GtkBgBox with ref=1.
 *            The caller is responsible for adding it to a container.
 **********************************************************************/

static gboolean fb_button_cross(GtkImage *widget, GdkEventCrossing *event);
static gboolean fb_button_pressed(GtkWidget *widget, GdkEventButton *event);

/*
 * fb_button_new - Create an fb_button composite widget.
 *
 * Constructs a GtkBgBox containing an fb_image, wires up hover and press
 * event handlers, and pre-generates the hover (pix[1]) and press (pix[2])
 * pixbuf variants.
 *
 * @iname   : Icon theme name (passed to fb_image_new), or NULL.
 * @fname   : File path (passed to fb_image_new), or NULL.
 * @width   : Desired icon width in pixels.
 * @height  : Desired icon height in pixels.
 * @hicolor : 24-bit highlight colour (0x00RRGGBB) for mouse-over brightening.
 *            Pass 0 to skip brightening (produces identical copy for pix[1]).
 * @label   : Currently IGNORED.  The FIXME comment in the original code
 *            acknowledges this.
 *
 * Returns: A GtkWidget* (GtkBgBox) containing the image.  Ref-count = 1.
 *          Already shown (gtk_widget_show_all called).
 *          Caller should add to a container or manage the ref.
 *
 * Memory: The image widget is added to the bgbox container (transferred
 *         ownership).  conf->pix[1] and conf->pix[2] are stored in the
 *         fb_image's conf struct, which is freed by fb_image_free().
 *
 * Ref-count: fb_image_new() returns image with ref=1 (shown).
 *            gtk_container_add() takes ownership; caller must not unref image.
 *            The returned bgbox (b) has ref=1; the caller should add it to a
 *            container or call g_object_unref() when done.
 *
 * ISSUE: @label parameter is declared but never used (FIXME in original code).
 *
 * ISSUE: GTK_WIDGET_UNSET_FLAGS is deprecated; use gtk_widget_set_can_focus().
 */
GtkWidget *
fb_button_new(gchar *iname, gchar *fname, int width, int height,
      gulong hicolor, gchar *label)
{
    GtkWidget *b, *image;
    fb_image_conf_t *conf;

    ENTER;
    b = gtk_bgbox_new();                                    // pseudo-transparent container
    gtk_container_set_border_width(GTK_CONTAINER(b), 0);
    GTK_WIDGET_UNSET_FLAGS (b, GTK_CAN_FOCUS);             // deprecated flag clear
    image = fb_image_new(iname, fname, width, height);     // creates pix[0]
    gtk_misc_set_alignment(GTK_MISC(image), 0.5, 0.5);    // center image within its cell
    gtk_misc_set_padding (GTK_MISC(image), 0, 0);
    // Retrieve the image's private state to set hicolor and build hover/press pixbufs
    conf = g_object_get_data(G_OBJECT(image), "conf");
    conf->hicolor = hicolor;
    // Build hover pixbuf (brightened by hicolor)
    conf->pix[1] = fb_pixbuf_make_back_image(conf->pix[0], conf->hicolor);
    // Build press pixbuf (scaled-down version of the hover pixbuf)
    conf->pix[2] = fb_pixbuf_make_press_image(conf->pix[1]);
    // Add event masks so the bgbox receives crossing (hover) events
    gtk_widget_add_events(b, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    // Connect enter/leave hover events; g_signal_connect_swapped swaps instance and
    // user-data so that fb_button_cross receives (image, event) not (b, event)
    g_signal_connect_swapped (G_OBJECT (b), "enter-notify-event",
            G_CALLBACK (fb_button_cross), image);
    g_signal_connect_swapped (G_OBJECT (b), "leave-notify-event",
            G_CALLBACK (fb_button_cross), image);
    // Connect button press/release events similarly
    g_signal_connect_swapped (G_OBJECT (b), "button-release-event",
          G_CALLBACK (fb_button_pressed), image);
    g_signal_connect_swapped (G_OBJECT (b), "button-press-event",
          G_CALLBACK (fb_button_pressed), image);
    gtk_container_add(GTK_CONTAINER(b), image);  // transfers image ownership to b
    gtk_widget_show_all(b);                       // show the bgbox and all children
    RET(b);  // caller owns the bgbox
}


/*
 * fb_button_cross - Handle GDK_ENTER_NOTIFY or GDK_LEAVE_NOTIFY on an fb_button.
 *
 * Called (via g_signal_connect_swapped) when the mouse enters or leaves the
 * fb_button's GtkBgBox.  Swaps the GtkImage's displayed pixbuf between:
 *   pix[0] (normal)    when the mouse LEAVES
 *   pix[1] (hover)     when the mouse ENTERS
 *
 * @widget : The GtkImage (NOT the bgbox; swapped by g_signal_connect_swapped).
 * @event  : The crossing event carrying the event type (ENTER or LEAVE).
 *
 * Returns: TRUE to stop further event propagation.
 *
 * ISSUE: If pix[1] is NULL (because hicolor was 0 and allocation failed,
 *        or fb_image was not wrapped in fb_button), gtk_image_set_from_pixbuf()
 *        will receive NULL, which clears the image display.  Not a crash but
 *        visually undesirable.
 */
static gboolean
fb_button_cross(GtkImage *widget, GdkEventCrossing *event)
{
    fb_image_conf_t *conf;
    int i;

    ENTER;
    conf = g_object_get_data(G_OBJECT(widget), "conf");
    // Select pixbuf index: 0 for leave (normal), 1 for enter (hover)
    if (event->type == GDK_LEAVE_NOTIFY) {
        i = 0;  // show normal image when mouse leaves
    } else {
        i = 1;  // show hover image when mouse enters
    }
    // Only swap if the displayed pixbuf is actually changing (avoids redundant draws)
    if (conf->i != i) {
        conf->i = i;
        gtk_image_set_from_pixbuf(GTK_IMAGE(widget), conf->pix[i]);
    }
    DBG("%s/%s - %s - pix[%d]=%p\n", conf->iname, conf->fname,
	(event->type == GDK_LEAVE_NOTIFY) ? "out" : "in",
	conf->i, conf->pix[conf->i]);
    RET(TRUE);  // consume event – don't propagate further
}

/*
 * fb_button_pressed - Handle GDK_BUTTON_PRESS or GDK_BUTTON_RELEASE on an fb_button.
 *
 * Called (via g_signal_connect_swapped) when a mouse button is pressed or
 * released over the fb_button's GtkBgBox.  Swaps the displayed pixbuf between:
 *   pix[2] (pressed) when the button is DOWN
 *   pix[1] (hover)   when the button is released INSIDE the widget
 *   pix[0] (normal)  when the button is released OUTSIDE the widget
 *
 * @widget : The GtkImage (swapped by g_signal_connect_swapped).
 * @event  : The button event (carries type, button number, and coordinates).
 *
 * Returns: FALSE to allow further event propagation.  This is intentional:
 *          returning FALSE means the parent (or plugin) can still react to
 *          the button press via their own signal handlers.
 *
 * ISSUE: Coordinate check (event->x >= 0 && event->x < allocation.width)
 *        uses widget->allocation directly (deprecated direct struct access).
 *        Should use gtk_widget_get_allocation() in newer GTK2.
 *
 * ISSUE: This handler does not distinguish which mouse button was pressed
 *        (event->button).  Right-clicks will also trigger the press animation,
 *        even if the action tied to the right-click is handled elsewhere.
 */
static gboolean
fb_button_pressed(GtkWidget *widget, GdkEventButton *event)
{
    fb_image_conf_t *conf;
    int i;

    ENTER;
    conf = g_object_get_data(G_OBJECT(widget), "conf");
    if (event->type == GDK_BUTTON_PRESS) {
        i = 2;  // show "pressed" (shrunken) image on button down
    } else {
        // Button release: determine whether the cursor is still over the widget
        if ((event->x >=0 && event->x < widget->allocation.width)
                && (event->y >=0 && event->y < widget->allocation.height))
            i = 1;  // released inside – revert to hover image
        else
            i = 0;  // released outside – revert to normal image
    }
    // Only update if the pixbuf actually changes
    if (conf->i != i) {
        conf->i = i;
        gtk_image_set_from_pixbuf(GTK_IMAGE(widget), conf->pix[i]);
    }
    RET(FALSE);  // allow event propagation (so plugin handlers can act on the click)
}

