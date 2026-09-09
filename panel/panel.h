/**
 * @file
 * @brief Core panel type definitions, global variables, and shared enums.
 *
 * This is the central shared header included by every panel source file and
 * every plugin. It defines the `panel` struct (the entire panel runtime state),
 * all EWMH Atom declarations, layout enumerations, and key global variables.
 *
 * @par Include hierarchy
 *   panel.h includes bg.h, ev.h, xconf.h, config.h, and libintl.h.
 *   All plugins include plugin.h which includes panel.h and misc.h.
 *
 * @par Global variables exported here
 *   - ::fbev        -- the EWMH event bus (FbEv GObject)
 *   - ::icon_theme  -- the global GtkIconTheme (do not unref)
 *   - the_panel     -- pointer to the single panel instance (declared in panel.c)
 *   - ::verbose     -- debug verbosity level
 *   - ::force_quit  -- set to non-zero to exit the main loop
 *   - ::cprofile    -- currently active profile name string
 */
#ifndef PANEL_H
#define PANEL_H


#include <X11/Xlib.h>      /* Atom, Window, Display types */
#include <gtk/gtk.h>       /* GtkWidget, GtkOrientation, etc. */
#include <gdk/gdk.h>       /* GdkColor, GdkRectangle, GdkWindow */

#include <libintl.h>
/** @def _(String)
 *  @brief Marks a string for gettext translation. */
#define _(String) gettext(String)
/** @def c_(String)
 *  @brief Same as #_ but for strings that should be translatable in future. */
#define c_(String) String

#include "config.h"   /* cmake-generated: PREFIX, LIBDIR, DATADIR, etc. */

#include "bg.h"       /* FbBg: root window background pixmap reader */
#include "ev.h"       /* FbEv: EWMH event signal bus */
#include "xconf.h"    /* xconf: configuration tree parser */

/* --- Panel layout enumerations --- */

/** @brief Panel alignment within its edge. */
enum { ALLIGN_CENTER, ALLIGN_LEFT, ALLIGN_RIGHT  };

/** @brief Panel docking edge. */
enum { EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP };

/** @brief Panel width specification type. */
enum { WIDTH_PERCENT,   /**< Width is a percentage of screen width. */
       WIDTH_REQUEST,   /**< Width determined by content request. */
       WIDTH_PIXEL };   /**< Width is an absolute pixel count. */

/** @brief Panel height specification type. */
enum { HEIGHT_PIXEL,    /**< Height is an absolute pixel count. */
       HEIGHT_REQUEST   /**< Height determined by content request. */
};

/** @brief Panel item positioning (unused/reserved). */
enum { POS_NONE, POS_START, POS_END };

/** @brief Autohide state machine states. */
enum { HIDDEN,    /**< Panel is in its minimised (hidden) state. */
       WAITING,   /**< Timer is running before hiding. */
       VISIBLE    /**< Panel is fully visible. */
};

/** @brief Panel Z-order layer (relative to normal windows). */
enum { LAYER_ABOVE, LAYER_BELOW };

/* --- Panel geometry constants --- */

#define PANEL_HEIGHT_DEFAULT  26    /**< Default panel height in pixels. */
#define PANEL_HEIGHT_MAX      200   /**< Maximum allowed panel height. */
#define PANEL_HEIGHT_MIN      16    /**< Minimum allowed panel height. */

/** @def IMGPREFIX
 *  @brief Path prefix for built-in panel images (set by cmake from DATADIR). */
#define IMGPREFIX  DATADIR "/images"

/**
 * @brief Runtime state for the single fbpanel instance.
 *
 * Created in panel.c:panel_new() and accessed via the global `the_panel`.
 * Plugins receive a non-owning pointer via plugin_instance::panel.
 */
typedef struct _panel
{
    GtkWidget *topgwin;           /**< Top-level panel GtkWindow. */
    Window topxwin;               /**< X11 window ID of topgwin (obtained after realise). */
    GtkWidget *lbox;              /**< Outer layout GtkBox filling topgwin. */
    GtkWidget *bbox;              /**< GtkBgbox providing the panel background. */
    GtkWidget *box;               /**< GtkHBox (or GtkVBox for vertical) containing all plugin pwids. */
    GtkWidget *menu;              /**< Right-click context GtkMenu. */
    GtkRequisition requisition;   /**< Last size request from the box. */
    /* Factory functions — set to gtk_hbox_new/gtk_vbox_new etc. at init */
    GtkWidget *(*my_box_new) (gboolean, gint);      /**< Box constructor (horizontal/vertical chosen at init). */
    GtkWidget *(*my_separator_new) ();              /**< Separator constructor. */

    /* Background / pseudo-transparency */
    FbBg *bg;                     /**< Root window pixmap reader (may be NULL). */
    int alpha;                    /**< Tint alpha: 0=opaque tint, 255=fully transparent. */
    guint32 tintcolor;            /**< Packed ARGB tint colour. */
    GdkColor gtintcolor;          /**< Same as #tintcolor, as a GdkColor for GTK use. */
    gchar *tintcolor_name;        /**< "#RRGGBB" string (g_strdup'd; g_free on destroy). */

    /* Panel geometry — preferred (ax/ay/aw/ah) and actual (cx/cy/cw/ch) */
    int ax, ay, aw, ah;           /**< Preferred panel position and size (set by calculate_position()). */
    int cx, cy, cw, ch;           /**< Current position and size (from configure events). */
    int allign, edge;             /**< ALLIGN_* and EDGE_* enum values. */
    int xmargin, ymargin;         /**< Margin from the alignment point, in pixels. */
    GtkOrientation orientation;   /**< Horizontal or vertical. */
    int widthtype, width;         /**< WIDTH_* enum and value. */
    int heighttype, height;       /**< HEIGHT_* enum and value. */
    int round_corners_radius;     /**< Reserved: rounded corner radius (not implemented). */
    int max_elem_height;          /**< Maximum plugin element height in pixels. */

    /* Multi-monitor */
    int xineramaHead;             /**< Target monitor index; -1 = use primary (see ::FBPANEL_INVALID_XINERAMA_HEAD). */
    GdkRectangle screenRect;      /**< Geometry of the target monitor. */

    /* Panel behaviour flags */
    gint self_destroy;            /**< Set to 1 to destroy panel on idle. */
    gint setdocktype;             /**< 1 = set _NET_WM_WINDOW_TYPE_DOCK. */
    gint setstrut;                /**< 1 = set _NET_WM_STRUT_PARTIAL. */
    gint round_corners;           /**< Reserved: 1 = draw rounded corners. */
    gint transparent;             /**< 1 = use pseudo-transparent background. */
    gint autohide;                /**< 1 = enable autohide. */
    gint ah_far;                  /**< 1 = mouse is far from panel edge. */
    gint layer;                   /**< LAYER_ABOVE or LAYER_BELOW. */
    gint setlayer;                /**< 1 = apply the layer hint to the WM. */

    /* Autohide state */
    int ah_dx, ah_dy;             /**< Pixel offsets for slide-hide animation. */
    int height_when_hidden;       /**< Panel height when hidden, in pixels (>= 1). */
    guint hide_tout;              /**< GLib source ID of hide-delay timer; 0 if off. */

    int spacing;                  /**< Pixel gap between plugins in the box. */

    /* EWMH state cache */
    guint desknum;                /**< _NET_NUMBER_OF_DESKTOPS. */
    guint curdesk;                /**< _NET_CURRENT_DESKTOP. */
    guint32 *workarea;            /**< _NET_WORKAREA array; g_malloc'd; may be NULL. */

    /* Plugin management */
    int plug_num;                 /**< Number of active plugin instances. */
    GList *plugins;               /**< GList of plugin_instance*, in display order. */

    /** Current autohide state handler, set by ah_start()/ah_stop(). */
    gboolean (*ah_state)(struct _panel *);

    xconf *xc;                    /**< Root of the parsed config tree (owned by the panel). */
} panel;


/**
 * @brief Bitfield decoded from the _NET_WM_STATE atom list.
 *
 * Read by get_net_wm_state() in misc.c; used by taskbar and wincmd
 * to filter windows by state.
 *
 * @note C bitfield order is implementation-defined; do not rely on the
 *       memory layout of this struct across compilers/platforms.
 */
typedef struct {
    unsigned int modal : 1;            /**< _NET_WM_STATE_MODAL */
    unsigned int sticky : 1;           /**< _NET_WM_STATE_STICKY */
    unsigned int maximized_vert : 1;   /**< _NET_WM_STATE_MAXIMIZED_VERT */
    unsigned int maximized_horz : 1;   /**< _NET_WM_STATE_MAXIMIZED_HORZ */
    unsigned int shaded : 1;           /**< _NET_WM_STATE_SHADED */
    unsigned int skip_taskbar : 1;     /**< _NET_WM_STATE_SKIP_TASKBAR */
    unsigned int skip_pager : 1;       /**< _NET_WM_STATE_SKIP_PAGER */
    unsigned int hidden : 1;           /**< _NET_WM_STATE_HIDDEN (minimised) */
    unsigned int fullscreen : 1;       /**< _NET_WM_STATE_FULLSCREEN */
    unsigned int above : 1;            /**< _NET_WM_STATE_ABOVE */
    unsigned int below : 1;            /**< _NET_WM_STATE_BELOW */
} net_wm_state;

/**
 * @brief Bitfield decoded from _NET_WM_WINDOW_TYPE.
 *
 * Read by get_net_wm_window_type() in misc.c; used by taskbar to
 * filter out non-normal windows (docks, dialogs, splash screens, etc.).
 */
typedef struct {
    unsigned int desktop : 1;   /**< _NET_WM_WINDOW_TYPE_DESKTOP */
    unsigned int dock : 1;      /**< _NET_WM_WINDOW_TYPE_DOCK */
    unsigned int toolbar : 1;   /**< _NET_WM_WINDOW_TYPE_TOOLBAR */
    unsigned int menu : 1;      /**< _NET_WM_WINDOW_TYPE_MENU */
    unsigned int utility : 1;   /**< _NET_WM_WINDOW_TYPE_UTILITY */
    unsigned int splash : 1;    /**< _NET_WM_WINDOW_TYPE_SPLASH */
    unsigned int dialog : 1;    /**< _NET_WM_WINDOW_TYPE_DIALOG */
    unsigned int normal : 1;    /**< _NET_WM_WINDOW_TYPE_NORMAL */
} net_wm_window_type;

/**
 * @brief Name/function-pointer pair for panel right-click menu items.
 *
 * The ::commands array (defined in panel.c) is terminated by { NULL, NULL }.
 */
typedef struct {
    char *name;          /**< Menu item label. */
    void (*cmd)(void);   /**< Function to call on selection. */
} command;

extern command commands[];     /**< Panel right-click menu commands; see panel.c. */

extern gchar *cprofile;        /**< Currently active profile name (e.g., "default"). */

/* --- X11 Atom declarations ---
 * All atoms are interned in misc.c:resolve_atoms() during fb_init().
 * They are declared extern here so every file including panel.h can use them
 * without additional includes or lookups.
 */
extern Atom a_UTF8_STRING;              /**< UTF8_STRING encoding atom. */
extern Atom a_XROOTPMAP_ID;            /**< _XROOTPMAP_ID: wallpaper pixmap ID. */

/* ICCCM atoms */
extern Atom a_WM_STATE;                /**< WM_STATE property. */
extern Atom a_WM_CLASS;                /**< WM_CLASS property. */
extern Atom a_WM_DELETE_WINDOW;        /**< WM_DELETE_WINDOW protocol. */
extern Atom a_WM_PROTOCOLS;            /**< WM_PROTOCOLS property. */

/* EWMH atoms — desktop management */
extern Atom a_NET_WORKAREA;            /**< _NET_WORKAREA */
extern Atom a_NET_CLIENT_LIST;         /**< _NET_CLIENT_LIST */
extern Atom a_NET_CLIENT_LIST_STACKING;/**< _NET_CLIENT_LIST_STACKING */
extern Atom a_NET_NUMBER_OF_DESKTOPS;  /**< _NET_NUMBER_OF_DESKTOPS */
extern Atom a_NET_CURRENT_DESKTOP;     /**< _NET_CURRENT_DESKTOP */
extern Atom a_NET_DESKTOP_NAMES;       /**< _NET_DESKTOP_NAMES */
extern Atom a_NET_DESKTOP_GEOMETRY;    /**< _NET_DESKTOP_GEOMETRY */

/* EWMH atoms — window management */
extern Atom a_NET_ACTIVE_WINDOW;       /**< _NET_ACTIVE_WINDOW */
extern Atom a_NET_CLOSE_WINDOW;        /**< _NET_CLOSE_WINDOW client message. */
extern Atom a_NET_SUPPORTED;           /**< _NET_SUPPORTED: list of supported atoms. */

/* EWMH atoms — window state */
extern Atom a_NET_WM_STATE;                /**< _NET_WM_STATE property. */
extern Atom a_NET_WM_STATE_SKIP_TASKBAR;   /**< Don't show in taskbar. */
extern Atom a_NET_WM_STATE_SKIP_PAGER;     /**< Don't show in pager. */
extern Atom a_NET_WM_STATE_STICKY;         /**< Show on all desktops. */
extern Atom a_NET_WM_STATE_HIDDEN;         /**< Window is minimised. */
extern Atom a_NET_WM_STATE_SHADED;         /**< Window is shaded (title bar only). */
extern Atom a_NET_WM_STATE_ABOVE;          /**< Keep above other windows. */
extern Atom a_NET_WM_STATE_BELOW;          /**< Keep below other windows. */

/* Constants for _NET_WM_STATE client message action field */
#define a_NET_WM_STATE_REMOVE        0    /**< Remove/unset property. */
#define a_NET_WM_STATE_ADD           1    /**< Add/set property. */
#define a_NET_WM_STATE_TOGGLE        2    /**< Toggle property. */

/* EWMH atoms — window type */
extern Atom a_NET_WM_WINDOW_TYPE;              /**< _NET_WM_WINDOW_TYPE */
extern Atom a_NET_WM_WINDOW_TYPE_DESKTOP;      /**< Desktop background window. */
extern Atom a_NET_WM_WINDOW_TYPE_DOCK;         /**< Dock/panel window (fbpanel uses this). */
extern Atom a_NET_WM_WINDOW_TYPE_TOOLBAR;      /**< Toolbar window. */
extern Atom a_NET_WM_WINDOW_TYPE_MENU;         /**< Menu window. */
extern Atom a_NET_WM_WINDOW_TYPE_UTILITY;      /**< Utility window. */
extern Atom a_NET_WM_WINDOW_TYPE_SPLASH;       /**< Splash screen. */
extern Atom a_NET_WM_WINDOW_TYPE_DIALOG;       /**< Dialog window. */
extern Atom a_NET_WM_WINDOW_TYPE_NORMAL;       /**< Normal application window. */

/* EWMH atoms — window properties */
extern Atom a_NET_WM_DESKTOP;         /**< _NET_WM_DESKTOP: which desktop a window is on. */
extern Atom a_NET_WM_NAME;            /**< _NET_WM_NAME: UTF-8 window title. */
extern Atom a_NET_WM_VISIBLE_NAME;    /**< _NET_WM_VISIBLE_NAME: taskbar display title. */
extern Atom a_NET_WM_STRUT;           /**< _NET_WM_STRUT: reserved screen edges (old). */
extern Atom a_NET_WM_STRUT_PARTIAL;   /**< _NET_WM_STRUT_PARTIAL: reserved edges (new). */
extern Atom a_NET_WM_ICON;            /**< _NET_WM_ICON: ARGB icon data. */

/* KDE-specific atom */
extern Atom a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR; /**< KDE tray icon association. */


/* --- Config enum tables (defined in panel.c or misc.c) --- */
extern xconf_enum allign_enum[];      /**< "left"/"center"/"right" -> ALLIGN_*. */
extern xconf_enum edge_enum[];        /**< "top"/"bottom"/"left"/"right" -> EDGE_*. */
extern xconf_enum widthtype_enum[];   /**< "percent"/"pixel"/"request" -> WIDTH_*. */
extern xconf_enum heighttype_enum[];  /**< "pixel"/"request" -> HEIGHT_*. */
extern xconf_enum bool_enum[];        /**< "true"/"false"/"yes"/"no"/"1"/"0" -> int. */
extern xconf_enum pos_enum[];         /**< "none"/"start"/"end" -> POS_*. */
extern xconf_enum layer_enum[];       /**< "above"/"below" -> LAYER_*. */

/* --- Panel global state --- */
extern int verbose;                   /**< Debug verbosity; set via the --log command-line arg. */
extern gint force_quit;               /**< Non-zero to exit gtk_main(). */
extern FbEv *fbev;                    /**< Global EWMH event bus; created in fb_init(). */
extern GtkIconTheme *icon_theme;      /**< Global icon theme; do NOT g_object_unref(). */

/**
 * @def FBPANEL_WIN(win)
 * @brief Test if an X11 Window ID belongs to the panel.
 *
 * @param win The X11 Window ID to test.
 * @return The GdkWindow* for @p win if it is a panel window (i.e. if GDK
 *         knows about it), or NULL if it is an external window. Used by
 *         the taskbar to skip the panel window when building the task list.
 * @note gdk_window_lookup() searches GDK's internal window table by XID.
 *       It does not do an X11 roundtrip.
 */
#define FBPANEL_WIN(win)  gdk_window_lookup(win)

/* --- Panel public API --- */

/**
 * @brief Set _NET_WM_STRUT and _NET_WM_STRUT_PARTIAL on the panel window.
 *
 * Called after geometry changes to inform the WM of the reserved screen
 * area. The WM uses struts to avoid tiling maximised windows over the panel.
 *
 * @param p The panel whose geometry determines the strut values.
 */
void panel_set_wm_strut(panel *p);

/**
 * @brief Return the active profile name (e.g., "default").
 *
 * @return A string owned by the panel; do NOT free.
 */
gchar *panel_get_profile(void);

/**
 * @brief Return the full path to the active profile config file.
 *
 * @return A newly allocated string; caller must g_free().
 */
gchar *panel_get_profile_file(void);

/**
 * @brief Enable autohide on panel @p p.
 *
 * Connects enter/leave signals and starts the hide timer.
 *
 * @param p The panel to enable autohide on.
 */
void ah_start(panel *p);

/**
 * @brief Disable autohide on panel @p p.
 *
 * Disconnects signals, removes the timer, and restores the panel to
 * fully visible.
 *
 * @param p The panel to disable autohide on.
 */
void ah_stop(panel *p);

/** @def FBPANEL_INVALID_XINERAMA_HEAD
 *  @brief Sentinel value for `xineramaHead` meaning "use the primary monitor". */
#define FBPANEL_INVALID_XINERAMA_HEAD (-1)

#endif
