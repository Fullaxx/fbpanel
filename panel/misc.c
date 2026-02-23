
/*
 * misc.c - Miscellaneous utilities for fbpanel.
 *
 * After the misc.c split, this file retains only the pieces that don't
 * belong to the X11/EWMH layer (ewmh.c) or the widget factory (fbwidgets.c):
 *
 *   - Enum tables used by the config reader/writer (xconf.c, gconf_panel.c).
 *   - str2num / num2str: enum table lookup helpers.
 *   - fb_init / fb_free: one-time panel utility initialisation.
 *   - calculate_position: panel geometry computation.
 *   - expand_tilda: tilde-expansion for config file paths.
 *   - menu_pos: GtkMenuPositionFunc for the panel context menu.
 *   - indent: config-file indentation string helper.
 *
 * Memory ownership conventions used throughout this file:
 *   - Functions whose names end with "_new" or "_dup" return a newly allocated
 *     object that the caller owns and must free (g_free / g_object_unref).
 */

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <glib.h>

#include "misc.h"
#include "fbwidgets.h"
#include "ewmh.h"

//#define DEBUGPRN
#include "dbg.h"

/* Global reference to the single panel instance.  Defined in panel.c and
 * used here by menu_pos() to query the panel's geometry.
 * ISSUE: This hard-codes single-panel support; multi-panel setups would
 * require passing the panel explicitly. */
extern panel *the_panel;

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
