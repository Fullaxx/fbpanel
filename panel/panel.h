/*
 * panel.h -- Core panel type definitions, global variables, and shared enums.
 *
 * This is the central shared header included by every panel source file and
 * every plugin. It defines the `panel` struct (the entire panel runtime state),
 * all EWMH Atom declarations, layout enumerations, and key global variables.
 *
 * Include hierarchy:
 *   panel.h includes bg.h, ev.h, xconf.h, config.h, and libintl.h.
 *   All plugins include plugin.h which includes panel.h and misc.h.
 *
 * Global variables exported here:
 *   fbev        - the EWMH event bus (FbEv GObject)
 *   icon_theme  - the global GtkIconTheme (do not unref)
 *   the_panel   - pointer to the single panel instance (declared in panel.c)
 *   verbose     - debug verbosity level
 *   force_quit  - set to non-zero to exit the main loop
 *   cprofile    - currently active profile name string
 */
#ifndef PANEL_H
#define PANEL_H


#include <X11/Xlib.h>      /* Atom, Window, Display types */
#include <gtk/gtk.h>       /* GtkWidget, GtkOrientation, etc. */
#include <gdk/gdk.h>       /* GdkColor, GdkRectangle, GdkWindow */

#include <libintl.h>
/* _(str): marks a string for gettext translation */
#define _(String) gettext(String)
/* c_(str): same as _ but for strings that should be translatable in future */
#define c_(String) String

#include "config.h"   /* cmake-generated: PREFIX, LIBDIR, DATADIR, etc. */

#include "bg.h"       /* FbBg: root window background pixmap reader */
#include "ev.h"       /* FbEv: EWMH event signal bus */
#include "xconf.h"    /* xconf: configuration tree parser */

/* --- Panel layout enumerations --- */

/* Panel alignment within its edge */
enum { ALLIGN_CENTER, ALLIGN_LEFT, ALLIGN_RIGHT  };

/* Panel docking edge */
enum { EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP };

/* Panel width specification type */
enum { WIDTH_PERCENT,   /* width is a percentage of screen width */
       WIDTH_REQUEST,   /* width determined by content request */
       WIDTH_PIXEL };   /* width is an absolute pixel count */

/* Panel height specification type */
enum { HEIGHT_PIXEL,    /* height is an absolute pixel count */
       HEIGHT_REQUEST   /* height determined by content request */
};

/* Panel item positioning (unused/reserved) */
enum { POS_NONE, POS_START, POS_END };

/* Autohide state machine states */
enum { HIDDEN,    /* panel is in its minimised (hidden) state */
       WAITING,   /* timer is running before hiding */
       VISIBLE    /* panel is fully visible */
};

/* Panel Z-order layer (relative to normal windows) */
enum { LAYER_ABOVE, LAYER_BELOW };

/* --- Panel geometry constants --- */

#define PANEL_HEIGHT_DEFAULT  26    /* default panel height in pixels */
#define PANEL_HEIGHT_MAX      200   /* maximum allowed panel height */
#define PANEL_HEIGHT_MIN      16    /* minimum allowed panel height */

/* Path prefix for built-in panel images (set by cmake from DATADIR) */
#define IMGPREFIX  DATADIR "/images"

/*
 * struct _panel -- runtime state for the single fbpanel instance.
 *
 * Created in panel.c:panel_new() and accessed via the global `the_panel`.
 * Plugins receive a non-owning pointer via plugin_instance::panel.
 *
 * Layout-related fields:
 *   topgwin         - top-level GtkWindow (the panel bar itself)
 *   topxwin         - X11 window ID of topgwin (obtained after realise)
 *   lbox            - outer GtkBox that fills topgwin
 *   bbox            - GtkBgbox providing the panel background
 *   box             - GtkHBox (or GtkVBox for vertical) containing all plugins
 *   menu            - right-click context GtkMenu
 *
 * Geometry fields:
 *   ax, ay, aw, ah  - preferred allocation (set by calculate_position)
 *   cx, cy, cw, ch  - actual current allocation (from configure-event)
 *   allign          - ALLIGN_* enum value
 *   edge            - EDGE_* enum value
 *   xmargin, ymargin - margin from the alignment point (pixels)
 *   orientation     - GTK_ORIENTATION_HORIZONTAL or VERTICAL
 *   widthtype, width  - WIDTH_* enum and value
 *   heighttype, height - HEIGHT_* enum and value
 *
 * Appearance fields:
 *   bg              - FbBg* for root window pixmap (pseudo-transparency)
 *   alpha           - tint alpha 0-255 (0 = opaque tint)
 *   tintcolor       - ARGB tint colour (as guint32)
 *   gtintcolor      - same as GdkColor
 *   tintcolor_name  - tint colour as "#RRGGBB" string (g_strdup'd)
 *   transparent     - non-zero if pseudo-transparency is active
 *
 * Autohide fields:
 *   autohide        - non-zero if autohide is enabled
 *   ah_far          - non-zero if mouse is far from panel (hide condition)
 *   ah_dx, ah_dy    - pixel offsets applied when sliding the panel hidden
 *   height_when_hidden - panel height in pixels when hidden (≥ 1)
 *   hide_tout       - GLib timer source ID for the hide delay; 0 if inactive
 *   ah_state        - function pointer to the current autohide state handler
 *
 * Plugin management:
 *   plug_num        - count of active plugin instances
 *   plugins         - GList of plugin_instance* pointers (in display order)
 *   xc              - root of the parsed config tree (owned by the panel)
 *
 * Multimonitor:
 *   xineramaHead    - monitor index (-1 = FBPANEL_INVALID_XINERAMA_HEAD)
 *   screenRect      - GdkRectangle of the target monitor geometry
 *
 * EWMH state cache:
 *   desknum         - total number of virtual desktops
 *   curdesk         - current active desktop index
 *   workarea        - array of _NET_WORKAREA values (per-desktop [x,y,w,h])
 */
typedef struct _panel
{
    GtkWidget *topgwin;           /* top-level panel GtkWindow */
    Window topxwin;               /* X11 window ID of topgwin */
    GtkWidget *lbox;              /* outer layout GtkBox filling topgwin */
    GtkWidget *bbox;              /* GtkBgbox providing the panel background */
    GtkWidget *box;               /* GtkHBox/GtkVBox containing all plugin pwids */
    GtkWidget *menu;              /* right-click context menu */
    GtkRequisition requisition;   /* last size request from the box */
    /* Factory functions — set to gtk_hbox_new/gtk_vbox_new etc. at init */
    GtkWidget *(*my_box_new) (gboolean, gint);      /* box constructor */
    GtkWidget *(*my_separator_new) ();              /* separator constructor */

    /* Background / pseudo-transparency */
    FbBg *bg;                     /* root window pixmap reader (may be NULL) */
    int alpha;                    /* tint alpha: 0=opaque tint, 255=fully transparent */
    guint32 tintcolor;            /* packed ARGB tint colour */
    GdkColor gtintcolor;          /* same as GdkColor for GTK use */
    gchar *tintcolor_name;        /* "#RRGGBB" string (g_strdup'd; g_free on destroy) */

    /* Panel geometry — preferred (ax/ay/aw/ah) and actual (cx/cy/cw/ch) */
    int ax, ay, aw, ah;           /* preferred panel position and size */
    int cx, cy, cw, ch;           /* current position and size (from configure events) */
    int allign, edge;             /* ALLIGN_* and EDGE_* enum values */
    int xmargin, ymargin;         /* margin from alignment point in pixels */
    GtkOrientation orientation;   /* horizontal or vertical */
    int widthtype, width;         /* WIDTH_* and width value */
    int heighttype, height;       /* HEIGHT_* and height value */
    int round_corners_radius;     /* reserved: rounded corner radius (not implemented) */
    int max_elem_height;          /* maximum plugin element height in pixels */

    /* Multi-monitor */
    int xineramaHead;             /* target monitor index; -1 = use primary */
    GdkRectangle screenRect;      /* geometry of the target monitor */

    /* Panel behaviour flags */
    gint self_destroy;            /* set to 1 to destroy panel on idle */
    gint setdocktype;             /* 1 = set _NET_WM_WINDOW_TYPE_DOCK */
    gint setstrut;                /* 1 = set _NET_WM_STRUT_PARTIAL */
    gint round_corners;           /* reserved: 1 = draw rounded corners */
    gint transparent;             /* 1 = use pseudo-transparent background */
    gint autohide;                /* 1 = enable autohide */
    gint ah_far;                  /* 1 = mouse is far from panel edge */
    gint layer;                   /* LAYER_ABOVE or LAYER_BELOW */
    gint setlayer;                /* 1 = apply the layer hint to the WM */

    /* Autohide state */
    int ah_dx, ah_dy;             /* pixel offsets for slide-hide animation */
    int height_when_hidden;       /* panel height when hidden (pixels; ≥ 1) */
    guint hide_tout;              /* GLib source ID of hide-delay timer; 0 if off */

    int spacing;                  /* pixel gap between plugins in the box */

    /* EWMH state cache */
    guint desknum;                /* _NET_NUMBER_OF_DESKTOPS */
    guint curdesk;                /* _NET_CURRENT_DESKTOP */
    guint32 *workarea;            /* _NET_WORKAREA array; g_malloc'd; may be NULL */

    /* Plugin management */
    int plug_num;                 /* number of active plugin instances */
    GList *plugins;               /* GList of plugin_instance*; in display order */

    /* Autohide state function pointer (set by ah_start/ah_stop) */
    gboolean (*ah_state)(struct _panel *);

    xconf *xc;                    /* root of the config tree (owned by panel) */
} panel;


/*
 * net_wm_state -- bitfield decoded from the _NET_WM_STATE atom list.
 *
 * Read by get_net_wm_state() in misc.c; used by taskbar and wincmd
 * to filter windows by state.
 *
 * NOTE: C bitfield order is implementation-defined; do not rely on the
 * memory layout of this struct across compilers/platforms.
 */
typedef struct {
    unsigned int modal : 1;            /* _NET_WM_STATE_MODAL */
    unsigned int sticky : 1;           /* _NET_WM_STATE_STICKY */
    unsigned int maximized_vert : 1;   /* _NET_WM_STATE_MAXIMIZED_VERT */
    unsigned int maximized_horz : 1;   /* _NET_WM_STATE_MAXIMIZED_HORZ */
    unsigned int shaded : 1;           /* _NET_WM_STATE_SHADED */
    unsigned int skip_taskbar : 1;     /* _NET_WM_STATE_SKIP_TASKBAR */
    unsigned int skip_pager : 1;       /* _NET_WM_STATE_SKIP_PAGER */
    unsigned int hidden : 1;           /* _NET_WM_STATE_HIDDEN (minimised) */
    unsigned int fullscreen : 1;       /* _NET_WM_STATE_FULLSCREEN */
    unsigned int above : 1;            /* _NET_WM_STATE_ABOVE */
    unsigned int below : 1;            /* _NET_WM_STATE_BELOW */
} net_wm_state;

/*
 * net_wm_window_type -- bitfield decoded from _NET_WM_WINDOW_TYPE.
 *
 * Read by get_net_wm_window_type() in misc.c; used by taskbar to
 * filter out non-normal windows (docks, dialogs, splash screens, etc.).
 */
typedef struct {
    unsigned int desktop : 1;   /* _NET_WM_WINDOW_TYPE_DESKTOP */
    unsigned int dock : 1;      /* _NET_WM_WINDOW_TYPE_DOCK */
    unsigned int toolbar : 1;   /* _NET_WM_WINDOW_TYPE_TOOLBAR */
    unsigned int menu : 1;      /* _NET_WM_WINDOW_TYPE_MENU */
    unsigned int utility : 1;   /* _NET_WM_WINDOW_TYPE_UTILITY */
    unsigned int splash : 1;    /* _NET_WM_WINDOW_TYPE_SPLASH */
    unsigned int dialog : 1;    /* _NET_WM_WINDOW_TYPE_DIALOG */
    unsigned int normal : 1;    /* _NET_WM_WINDOW_TYPE_NORMAL */
} net_wm_window_type;

/*
 * command -- name/function-pointer pair for panel right-click menu items.
 *
 * The commands[] array (defined in panel.c) is terminated by { NULL, NULL }.
 */
typedef struct {
    char *name;          /* menu item label */
    void (*cmd)(void);   /* function to call on selection */
} command;

extern command commands[];     /* panel right-click menu commands; see panel.c */

extern gchar *cprofile;        /* currently active profile name (e.g., "default") */

/* --- X11 Atom declarations ---
 * All atoms are interned in misc.c:resolve_atoms() during fb_init().
 * They are declared extern here so every file including panel.h can use them
 * without additional includes or lookups.
 */
extern Atom a_UTF8_STRING;              /* UTF8_STRING encoding atom */
extern Atom a_XROOTPMAP_ID;            /* _XROOTPMAP_ID: wallpaper pixmap ID */

/* ICCCM atoms */
extern Atom a_WM_STATE;                /* WM_STATE property */
extern Atom a_WM_CLASS;                /* WM_CLASS property */
extern Atom a_WM_DELETE_WINDOW;        /* WM_DELETE_WINDOW protocol */
extern Atom a_WM_PROTOCOLS;            /* WM_PROTOCOLS property */

/* EWMH atoms — desktop management */
extern Atom a_NET_WORKAREA;            /* _NET_WORKAREA */
extern Atom a_NET_CLIENT_LIST;         /* _NET_CLIENT_LIST */
extern Atom a_NET_CLIENT_LIST_STACKING;/* _NET_CLIENT_LIST_STACKING */
extern Atom a_NET_NUMBER_OF_DESKTOPS;  /* _NET_NUMBER_OF_DESKTOPS */
extern Atom a_NET_CURRENT_DESKTOP;     /* _NET_CURRENT_DESKTOP */
extern Atom a_NET_DESKTOP_NAMES;       /* _NET_DESKTOP_NAMES */
extern Atom a_NET_DESKTOP_GEOMETRY;    /* _NET_DESKTOP_GEOMETRY */

/* EWMH atoms — window management */
extern Atom a_NET_ACTIVE_WINDOW;       /* _NET_ACTIVE_WINDOW */
extern Atom a_NET_CLOSE_WINDOW;        /* _NET_CLOSE_WINDOW client message */
extern Atom a_NET_SUPPORTED;           /* _NET_SUPPORTED: list of supported atoms */

/* EWMH atoms — window state */
extern Atom a_NET_WM_STATE;                /* _NET_WM_STATE property */
extern Atom a_NET_WM_STATE_SKIP_TASKBAR;   /* don't show in taskbar */
extern Atom a_NET_WM_STATE_SKIP_PAGER;     /* don't show in pager */
extern Atom a_NET_WM_STATE_STICKY;         /* show on all desktops */
extern Atom a_NET_WM_STATE_HIDDEN;         /* window is minimised */
extern Atom a_NET_WM_STATE_SHADED;         /* window is shaded (title bar only) */
extern Atom a_NET_WM_STATE_ABOVE;          /* keep above other windows */
extern Atom a_NET_WM_STATE_BELOW;          /* keep below other windows */

/* Constants for _NET_WM_STATE client message action field */
#define a_NET_WM_STATE_REMOVE        0    /* remove/unset property */
#define a_NET_WM_STATE_ADD           1    /* add/set property */
#define a_NET_WM_STATE_TOGGLE        2    /* toggle property */

/* EWMH atoms — window type */
extern Atom a_NET_WM_WINDOW_TYPE;              /* _NET_WM_WINDOW_TYPE */
extern Atom a_NET_WM_WINDOW_TYPE_DESKTOP;      /* desktop background window */
extern Atom a_NET_WM_WINDOW_TYPE_DOCK;         /* dock/panel window (fbpanel uses this) */
extern Atom a_NET_WM_WINDOW_TYPE_TOOLBAR;      /* toolbar window */
extern Atom a_NET_WM_WINDOW_TYPE_MENU;         /* menu window */
extern Atom a_NET_WM_WINDOW_TYPE_UTILITY;      /* utility window */
extern Atom a_NET_WM_WINDOW_TYPE_SPLASH;       /* splash screen */
extern Atom a_NET_WM_WINDOW_TYPE_DIALOG;       /* dialog window */
extern Atom a_NET_WM_WINDOW_TYPE_NORMAL;       /* normal application window */

/* EWMH atoms — window properties */
extern Atom a_NET_WM_DESKTOP;         /* _NET_WM_DESKTOP: which desktop a window is on */
extern Atom a_NET_WM_NAME;            /* _NET_WM_NAME: UTF-8 window title */
extern Atom a_NET_WM_VISIBLE_NAME;    /* _NET_WM_VISIBLE_NAME: taskbar display title */
extern Atom a_NET_WM_STRUT;           /* _NET_WM_STRUT: reserved screen edges (old) */
extern Atom a_NET_WM_STRUT_PARTIAL;   /* _NET_WM_STRUT_PARTIAL: reserved edges (new) */
extern Atom a_NET_WM_ICON;            /* _NET_WM_ICON: ARGB icon data */

/* KDE-specific atom */
extern Atom a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR; /* KDE tray icon association */


/* --- Config enum tables (defined in panel.c or misc.c) --- */
extern xconf_enum allign_enum[];      /* "left"/"center"/"right" → ALLIGN_* */
extern xconf_enum edge_enum[];        /* "top"/"bottom"/"left"/"right" → EDGE_* */
extern xconf_enum widthtype_enum[];   /* "percent"/"pixel"/"request" → WIDTH_* */
extern xconf_enum heighttype_enum[];  /* "pixel"/"request" → HEIGHT_* */
extern xconf_enum bool_enum[];        /* "true"/"false"/"yes"/"no"/"1"/"0" → int */
extern xconf_enum pos_enum[];         /* "none"/"start"/"end" → POS_* */
extern xconf_enum layer_enum[];       /* "above"/"below" → LAYER_* */

/* --- Panel global state --- */
extern int verbose;                   /* debug verbosity; set via --log command-line arg */
extern gint force_quit;               /* non-zero to exit gtk_main() */
extern FbEv *fbev;                    /* global EWMH event bus; created in fb_init() */
extern GtkIconTheme *icon_theme;      /* global icon theme; do NOT g_object_unref() */

/*
 * FBPANEL_WIN(win) -- test if an X11 Window ID belongs to the panel.
 *
 * Returns the GdkWindow* for 'win' if it is a panel window (i.e., if GDK
 * knows about it), or NULL if it is an external window.  Used by the taskbar
 * to skip the panel window when building the task list.
 *
 * Note: gdk_window_lookup() searches GDK's internal window table by XID.
 * It does not do an X11 roundtrip.
 */
#define FBPANEL_WIN(win)  gdk_window_lookup(win)

/* --- Panel public API --- */

/*
 * panel_set_wm_strut -- set _NET_WM_STRUT and _NET_WM_STRUT_PARTIAL on the panel window.
 *
 * Called after geometry changes to inform the WM of the reserved screen area.
 * The WM uses struts to avoid tiling maximised windows over the panel.
 */
void panel_set_wm_strut(panel *p);

/*
 * panel_get_profile -- return the active profile name (e.g., "default").
 *
 * Returns: a string owned by the panel; do NOT free.
 */
gchar *panel_get_profile(void);

/*
 * panel_get_profile_file -- return the full path to the active profile config file.
 *
 * Returns: a newly allocated string; caller must g_free().
 */
gchar *panel_get_profile_file(void);

/*
 * ah_start / ah_stop -- start and stop the autohide state machine.
 *
 * ah_start: enables autohide on panel p (connects enter/leave signals,
 *           starts the hide timer).
 * ah_stop:  disables autohide (disconnects signals, removes timer,
 *           restores the panel to fully visible).
 */
void ah_start(panel *p);
void ah_stop(panel *p);

/* Sentinel value for xineramaHead meaning "use the primary monitor" */
#define FBPANEL_INVALID_XINERAMA_HEAD (-1)

#endif
