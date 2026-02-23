/* dclock is an adaptation of blueclock by Jochen Baier <email@Jochen-Baier.de>
 *
 * This plugin renders a digital clock using a custom bitmap glyph image
 * (dclock_glyphs.png) rather than GTK text rendering. It supports both
 * horizontal (row of digits) and vertical (stacked digit-pair) layouts,
 * 12/24h time formats, optional seconds display, configurable color,
 * a click action (or toggle-calendar fallback), and a date tooltip.
 *
 * Timer: a 1-second g_timeout_add fires clock_update() continuously.
 * Memory: dc->clock (GdkPixbuf) is allocated in dclock_create_pixbufs and
 *         must NOT be explicitly freed here because GTK holds a reference
 *         through the GtkImage widget; dc->glyphs must be freed manually
 *         but the destructor does not do so -- see BUG below.
 */

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>


#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"


/* strftime format used for the tooltip -- shows weekday and locale date */
#define TOOLTIP_FMT    "%A %x"

/* Legacy defines kept for reference; actual values come from the new macros */
#define CLOCK_24H_FMT  "%R"
#define CLOCK_12H_FMT  "%I:%M"

/* Pixel-art glyph dimensions within dclock_glyphs.png.
 * Each glyph slot is 20px wide; digits are DIGIT_WIDTH x DIGIT_HEIGHT,
 * the colon glyph occupies COLON_WIDTH x DIGIT_HEIGHT when horizontal,
 * or VCOLON_WIDTH x VCOLON_HEIGHT when vertical. */
#define CLOCK_24H_FMT     "%R"
#define CLOCK_24H_SEC_FMT "%T"
#define CLOCK_12H_FMT     "%I:%M"
#define CLOCK_12H_SEC_FMT "%I:%M:%S"

#define COLON_WIDTH   7   /* horizontal colon rendered width in pixels */
#define COLON_HEIGHT  5   /* unused -- DIGIT_HEIGHT used instead       */
#define VCOLON_WIDTH   10 /* vertical colon rendered width              */
#define VCOLON_HEIGHT  6  /* vertical colon rendered height             */
#define DIGIT_WIDTH   11  /* single digit glyph width                  */
#define DIGIT_HEIGHT  15  /* single digit glyph height                 */
#define SHADOW 2          /* left/top margin (shadow offset) in pixels */

/* Maximum size of strftime output buffer */
#define STR_SIZE  64

/* Enumeration for 12/24 hour display mode */
enum { DC_24H, DC_12H };

/* Configuration mapping for the HoursView xconf key */
xconf_enum hours_view_enum[] = {
    { .num = DC_24H, .str = "24" },
    { .num = DC_12H, .str = "12" },
    { .num = 0, .str = NULL },      /* sentinel -- must be last */
};

/* Per-instance private data for the dclock plugin.
 * Embedded inside the plugin_instance union, so the plugin framework
 * allocates sizeof(dclock_priv) bytes for each instance.
 *
 * Memory ownership:
 *   glyphs  -- owned here; NOT freed in destructor (bug).
 *   clock   -- owned by GtkImage after gtk_image_new_from_pixbuf; do NOT
 *              g_object_unref separately unless you also drop the widget ref.
 *   tfmt    -- points into xconf storage; do not free.
 *   cfmt    -- points to a string literal; do not free.
 *   action  -- points into xconf storage; do not free.
 */
typedef struct
{
    plugin_instance plugin;       /* must be first -- base class fields        */
    GtkWidget *main;              /* GtkImage displaying dc->clock pixbuf      */
    GtkWidget *calendar_window;   /* popup calendar window, or NULL            */
    gchar *tfmt, tstr[STR_SIZE];  /* tooltip strftime format & last rendered   */
    gchar *cfmt, cstr[STR_SIZE];  /* clock  strftime format & last rendered    */
    char *action;                 /* optional shell command on click           */
    int timer;                    /* g_timeout_add handle, 0 = not running     */
    GdkPixbuf *glyphs;            /* source glyph sheet: vert row of '0'-'9', ':' */
    GdkPixbuf *clock;             /* destination pixbuf rendered into GtkImage */
    guint32 color;                /* AARRGGBB glyph color (default: opaque black) */
    gboolean show_seconds;        /* TRUE if seconds digit pair is shown       */
    gboolean hours_view;          /* DC_24H or DC_12H                          */
    GtkOrientation orientation;   /* copied from panel at construction time    */
} dclock_priv;

//static dclock_priv me;  /* left from single-instance era; no longer used */

/*
 * dclock_create_calendar -- create and return a transient popup calendar window.
 *
 * Returns: GtkWidget* (GtkWindow) containing a GtkCalendar.
 *          Caller owns the widget; destroy it with gtk_widget_destroy().
 *
 * The window is positioned near the mouse pointer, is undecorated, sticky
 * (visible on all desktops), and excluded from the taskbar and pager.
 * No calendar_window pointer is stored here; the caller (clicked()) stores it.
 */
static GtkWidget *
dclock_create_calendar()
{
    GtkWidget *calendar, *win;

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 180, 180);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);     // no title bar
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(win), 5);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE); // hide in taskbar
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);   // hide in pager
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_MOUSE);
    gtk_window_set_title(GTK_WINDOW(win), "calendar");
    gtk_window_stick(GTK_WINDOW(win)); // show on all virtual desktops

    calendar = gtk_calendar_new();
    // Display week numbers, day name header row, and month/year heading
    gtk_calendar_display_options(
        GTK_CALENDAR(calendar),
        GTK_CALENDAR_SHOW_WEEK_NUMBERS | GTK_CALENDAR_SHOW_DAY_NAMES
        | GTK_CALENDAR_SHOW_HEADING);
    gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(calendar));

    return win; // caller takes ownership
}

/*
 * clicked -- GdkEventButton callback for mouse clicks on the plugin widget.
 *
 * Parameters:
 *   widget  -- the event box or plugin container receiving the click
 *   event   -- button event details
 *   dc      -- dclock_priv instance
 *
 * Returns: TRUE to consume the event, FALSE to propagate it.
 *
 * Behaviour:
 *   - Control+RightClick: propagates to panel (FALSE) for panel context menu.
 *   - If dc->action is set: runs the shell command asynchronously.
 *   - Otherwise: toggles the popup calendar window on/off.
 *     When the calendar is open, the tooltip is suppressed (set to NULL) so it
 *     does not overlap the calendar popup.
 *
 * Signal connection: "button_press_event" on p->pwid.
 * No disconnection needed; widget lifetime bounds the signal.
 */
static gboolean
clicked(GtkWidget *widget, GdkEventButton *event, dclock_priv *dc)
{
    ENTER;
    // Pass Control+RightClick through so panel can show its own context menu
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
            && event->state & GDK_CONTROL_MASK)
    {
        RET(FALSE);
    }
    if (dc->action != NULL)
        // Run user-configured command; errors are silently ignored
        g_spawn_command_line_async(dc->action, NULL);
    else
    {
        if (dc->calendar_window == NULL)
        {
            // First click: create and show the calendar popup
            dc->calendar_window = dclock_create_calendar();
            gtk_widget_show_all(dc->calendar_window);
            // While calendar is open, clear the tooltip to avoid overlap
            gtk_widget_set_tooltip_markup(dc->plugin.pwid, NULL);
        }
        else
        {
            // Second click: destroy the calendar window
            gtk_widget_destroy(dc->calendar_window);
            // NOTE: GTK will emit "destroy" and the widget is gone; set to NULL
            // to avoid a dangling pointer. clock_update() checks this field.
            dc->calendar_window = NULL;
        }
    }
    RET(TRUE);
}

/*
 * clock_update -- periodic 1-second timer callback; updates the clock image
 *                 and tooltip.
 *
 * Parameters:
 *   dc -- dclock_priv instance cast from the GSourceFunc gpointer.
 *
 * Returns: TRUE (keep the timer alive), always.
 *
 * Algorithm:
 *   1. Call strftime with dc->cfmt to get the current time string.
 *   2. If the string has changed since last update, re-render the glyph pixbuf
 *      into dc->clock character-by-character using gdk_pixbuf_copy_area.
 *      Digit glyphs are at offsets 0*20 through 9*20 in dc->glyphs.
 *      The colon glyph is at offset 10*20.
 *   3. Queue a GTK redraw of dc->main (the GtkImage).
 *   4. Separately, update the tooltip once per day (or clear it while the
 *      calendar popup is open).
 *
 * Potential issue: if strftime returns 0 (buffer too small or empty format),
 * the fallback "  :  " is written to output. The dc->cstr copy uses strncpy
 * without explicitly null-terminating if the string fills exactly STR_SIZE-1
 * characters -- see BUG entry.
 */
static gint
clock_update(dclock_priv *dc)
{
    char output[STR_SIZE], *tmp, *utf8;
    time_t now;
    struct tm * detail;
    int i, x, y;

    ENTER;
    time(&now);
    detail = localtime(&now); // NOTE: returns pointer to static buffer -- not thread-safe

    // Format the clock face string; fall back to "  :  " on failure
    if (!strftime(output, sizeof(output), dc->cfmt, detail))
        strcpy(output, "  :  ");
    // Only re-render the pixbuf if the displayed time actually changed
    if (strcmp(dc->cstr, output))
    {
        // Save new rendered string for next comparison
        strncpy(dc->cstr, output, sizeof(dc->cstr));
        // BUG: strncpy does not null-terminate if strlen(output) == STR_SIZE
        x = y = SHADOW; // start drawing after the shadow offset
        for (tmp = output; *tmp; tmp++)
        {
            DBGE("%c", *tmp);
            if (isdigit(*tmp))
            {
                // Map '0'-'9' to glyph column index 0-9; each glyph is 20px wide
                i = *tmp - '0';
                gdk_pixbuf_copy_area(dc->glyphs, i * 20, 0,
                    DIGIT_WIDTH, DIGIT_HEIGHT,
                    dc->clock, x, y);
                x += DIGIT_WIDTH; // advance cursor right by digit width
            }
            else if (*tmp == ':')
            {
                if (dc->orientation == GTK_ORIENTATION_HORIZONTAL) {
                    // Horizontal mode: draw colon glyph inline, offset 2px down
                    gdk_pixbuf_copy_area(dc->glyphs, 10 * 20, 0,
                        COLON_WIDTH, DIGIT_HEIGHT - 2,
                        dc->clock, x, y + 2);
                    x += COLON_WIDTH;
                } else {
                    // Vertical mode: reset x, move y down past the digit row,
                    // draw the colon glyph centred horizontally, then advance y
                    x = SHADOW;
                    y += DIGIT_HEIGHT;
                    gdk_pixbuf_copy_area(dc->glyphs, 10 * 20, 0,
                        VCOLON_WIDTH, VCOLON_HEIGHT,
                        dc->clock, x + DIGIT_WIDTH / 2, y);
                    y += VCOLON_HEIGHT;
                }
            }
            else
            {
                // Only digits and ':' are expected; anything else is a format bug
                ERR("dclock: got %c while expecting for digit or ':'\n", *tmp);
            }
        }
        DBG("\n");
        // Schedule a GTK expose event so the GtkImage redraws
        gtk_widget_queue_draw(dc->main);
    }

    // --- Tooltip update ---
    // While the calendar popup is open we suppress the tooltip entirely;
    // also suppress if strftime produces an empty string (e.g. tfmt="").
    if (dc->calendar_window || !strftime(output, sizeof(output),
            dc->tfmt, detail))
        output[0] = 0;    // empty string means "no tooltip"
    if (strcmp(dc->tstr, output))
    {
        // Tooltip text changed; convert from locale encoding to UTF-8 for GTK
        strcpy(dc->tstr, output);
        if (dc->tstr[0] && (utf8 = g_locale_to_utf8(output, -1,
                    NULL, NULL, NULL)))
        {
            gtk_widget_set_tooltip_markup(dc->plugin.pwid, utf8);
            g_free(utf8); // g_locale_to_utf8 allocates; caller must free
        }
        else
            gtk_widget_set_tooltip_markup(dc->plugin.pwid, NULL);
    }
    RET(TRUE); // TRUE keeps the g_timeout_add timer running
}

/*
 * dclock_set_color -- recolour all opaque, non-black pixels in the glyph sheet.
 *
 * Parameters:
 *   glyphs -- the loaded RGBA GdkPixbuf from dclock_glyphs.png; modified in place.
 *   color  -- 32-bit packed colour as 0x00RRGGBB (alpha byte is ignored).
 *
 * Algorithm: iterate every pixel; skip transparent (alpha==0) and pure-black
 * (r==g==b==0) pixels, overwrite the RGB channels of everything else with the
 * desired colour. This keeps the anti-aliased edges intact as long as they are
 * not exactly black.
 *
 * WARNING: this mutates the source glyph buffer permanently for the lifetime of
 * the plugin instance. Only call once per construction.
 */
static void
dclock_set_color(GdkPixbuf *glyphs, guint32 color)
{
    guchar *p1, *p2;
    int w, h;
    guint r, g, b;

    ENTER;
    p1 = gdk_pixbuf_get_pixels(glyphs); // raw RGBA pixel data
    h = gdk_pixbuf_get_height(glyphs);
    // Unpack the RRGGBB colour value
    r = (color & 0x00ff0000) >> 16;
    g = (color & 0x0000ff00) >> 8;
    b = (color & 0x000000ff);
    DBG("%dx%d: %02x %02x %02x\n",
        gdk_pixbuf_get_width(glyphs), gdk_pixbuf_get_height(glyphs), r, g, b);
    while (h--)
    {
        // p1 advances by the full rowstride (may be > width*4 due to alignment)
        for (p2 = p1, w = gdk_pixbuf_get_width(glyphs); w; w--, p2 += 4)
        {
            DBG("here %02x %02x %02x %02x\n", p2[0], p2[1], p2[2], p2[3]);
            // Skip fully transparent pixels and pure-black pixels
            if (p2[3] == 0 || !(p2[0] || p2[1] || p2[2]))
                continue;
            // Rewrite the RGB channels; leave the alpha channel (p2[3]) intact
            p2[0] = r;
            p2[1] = g;
            p2[2] = b;
        }
        p1 += gdk_pixbuf_get_rowstride(glyphs); // advance to next row
    }
    DBG("here\n");
    RET();
}

/*
 * dclock_create_pixbufs -- allocate dc->clock with dimensions appropriate for
 *                          the current orientation and show_seconds setting.
 *
 * Parameters:
 *   dc -- dclock_priv instance; orientation, show_seconds, glyphs, and
 *         dc->plugin.panel->aw (available width) are read.
 *
 * Side-effects:
 *   dc->clock is allocated (caller responsible for eventual g_object_unref).
 *   dc->orientation may be changed from VERTICAL to HORIZONTAL if the panel
 *   is too narrow to fit the stacked layout.
 *
 * Pixel layout (horizontal):
 *   [SHADOW][D][D][:][D][D]  and optionally [:][ D][D]
 *   Total width  = SHADOW + 4*DIGIT_WIDTH + COLON_WIDTH [+ COLON_WIDTH + 2*DIGIT_WIDTH]
 *   Total height = SHADOW + DIGIT_HEIGHT
 *
 * Pixel layout (vertical): two-row layout, digits stacked.
 *   The colon glyph is rotated 270 degrees and blit back into dc->glyphs
 *   at offset (200, 0) — the same glyph slot used for ':' — so the vertical
 *   colon is a modified version of the horizontal one.
 *
 * BUG: the rotated colon pixbuf (ch/cv) is created and immediately freed but
 *   the rotation result is copied back into the ORIGINAL glyphs buffer at
 *   offset (200,0), permanently altering it. If show_seconds is later toggled
 *   (which this plugin does not support at runtime) the colon would already
 *   be rotated.
 */
static void
dclock_create_pixbufs(dclock_priv *dc)
{
    int width, height;
    GdkPixbuf *ch, *cv;

    ENTER;
    // Start with the shadow margin applied to both axes
    width = height = SHADOW;
    // Horizontal base: 4 digits (HH:MM) + 1 colon
    width += COLON_WIDTH + 4 * DIGIT_WIDTH;
    height += DIGIT_HEIGHT;
    if (dc->show_seconds)
        width += COLON_WIDTH + 2 * DIGIT_WIDTH; // add :SS section
    if (dc->orientation == GTK_ORIENTATION_VERTICAL) {
        DBG("width=%d height=%d aw=%d\n", width, height, dc->plugin.panel->aw);
        // If the horizontal layout fits within the panel width, use it instead
        if (width < dc->plugin.panel->aw) {
            dc->orientation = GTK_ORIENTATION_HORIZONTAL;
            goto done; // skip vertical-layout recalculation
        }
        // Recalculate for vertical (stacked) layout
        width = height = SHADOW;
        // Extract the 8x8 colon glyph from offset (200, 0) in the glyph sheet
        ch = gdk_pixbuf_new_subpixbuf(dc->glyphs, 200, 0, 8, 8);
        cv = gdk_pixbuf_rotate_simple(ch, 270); // rotate for vertical display
        // Blit the rotated colon back onto the original glyph sheet in-place
        // BUG: permanently modifies dc->glyphs at (0,0) within the subpixbuf ch,
        //      but ch is a subpixbuf so writes go to dc->glyphs offset (200,0).
        gdk_pixbuf_copy_area(cv, 0, 0, 8, 8, ch, 0, 0);
        g_object_unref(cv);
        g_object_unref(ch);
        // Vertical canvas: two rows of digits separated by the colon height
        height += DIGIT_HEIGHT * 2 + VCOLON_HEIGHT;
        width += DIGIT_WIDTH * 2;
        if (dc->show_seconds)
            height += VCOLON_HEIGHT + DIGIT_HEIGHT; // extra row for seconds
    }
done:
    // Allocate the destination pixbuf filled with transparent black
    dc->clock = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    DBG("width=%d height=%d\n", width, height);
    gdk_pixbuf_fill(dc->clock, 0); // clear to fully transparent
    RET();
}

/*
 * dclock_destructor -- release resources when the plugin is unloaded.
 *
 * Parameters:
 *   p -- plugin_instance pointer (upcast to dclock_priv internally).
 *
 * Cleanup:
 *   - Removes the 1-second timer via g_source_remove.
 *   - Destroys the main GtkImage widget (which drops the clock pixbuf ref).
 *
 * BUG: dc->glyphs is never g_object_unref'd here -- memory leak.
 * BUG: dc->calendar_window is not destroyed here if it was left open.
 */
static void
dclock_destructor(plugin_instance *p)
{
    dclock_priv *dc = (dclock_priv *)p;

    ENTER;
    // Stop the periodic timer before destroying the widget it references
    if (dc->timer)
        g_source_remove(dc->timer);
    gtk_widget_destroy(dc->main);
    // NOTE: dc->glyphs and dc->clock are NOT freed here (dc->clock is owned
    //       by the GtkImage; dc->glyphs leaks -- see bug list).
    RET();
}

/*
 * dclock_constructor -- initialise and display the digital clock plugin.
 *
 * Parameters:
 *   p -- plugin_instance allocated by the framework (sizeof dclock_priv bytes).
 *
 * Returns: 1 on success, 0 on failure (e.g. glyph PNG not found).
 *
 * Configuration keys (read via XCG macro from xconf):
 *   TooltipFmt  -- strftime format for tooltip (default TOOLTIP_FMT)
 *   ClockFmt    -- DEPRECATED: generates an error and is removed from config
 *   ShowSeconds -- bool: show :SS seconds pair (default false)
 *   HoursView   -- "12" | "24" (default "24")
 *   Action      -- shell command to run on click (default: none)
 *   Color       -- CSS-style colour string (default: opaque black)
 *
 * Signals connected:
 *   "button_press_event" on p->pwid -> clicked()
 *
 * Timer: dc->timer is a 1-second recurring g_timeout_add handle.
 *        Must be removed in dclock_destructor.
 */
static int
dclock_constructor(plugin_instance *p)
{
    gchar *color_str;
    dclock_priv *dc;
    //int width;  /* removed -- width now computed inside dclock_create_pixbufs */

    ENTER;
    DBG("dclock: use 'tclock' plugin for text version of a time and date\n");
    dc = (dclock_priv *) p;
    // Load the glyph sprite sheet; return failure if the file is missing
    dc->glyphs = gdk_pixbuf_new_from_file(IMGPREFIX "/dclock_glyphs.png", NULL);
    if (!dc->glyphs)
        RET(0); // caller (plugin loader) will free p

    // Defaults before reading user configuration
    dc->cfmt = NULL;
    dc->tfmt = TOOLTIP_FMT;
    dc->action = NULL;
    dc->color = 0xff000000; // opaque black (AARRGGBB)
    dc->show_seconds = FALSE;
    dc->hours_view = DC_24H;
    dc->orientation = p->panel->orientation; // inherit from panel
    color_str = NULL;
    XCG(p->xc, "TooltipFmt", &dc->tfmt, str);
    XCG(p->xc, "ClockFmt", &dc->cfmt, str);
    XCG(p->xc, "ShowSeconds", &dc->show_seconds, enum, bool_enum);
    XCG(p->xc, "HoursView", &dc->hours_view, enum, hours_view_enum);
    XCG(p->xc, "Action", &dc->action, str);
    XCG(p->xc, "Color", &color_str, str);
    if (dc->cfmt)
    {
        // ClockFmt was replaced by ShowSeconds + HoursView; warn and strip it
        ERR("dclock: ClockFmt option is deprecated. Please use\n"
            "following options instead\n"
            "  ShowSeconds = false | true\n"
            "  HoursView = 12 | 24\n");
        xconf_del(xconf_get(p->xc, "ClockFmt"), FALSE);
        dc->cfmt = NULL;
    }
    if (color_str)
    {
        // Parse CSS colour string (e.g. "red", "#ff0000") into GdkColor
        GdkColor color;
        if (gdk_color_parse (color_str, &color))
            dc->color = gcolor2rgb24(&color); // convert to 0x00RRGGBB packed int
    }
    // Select the strftime format based on user preferences
    if (dc->hours_view == DC_24H)
        dc->cfmt = (dc->show_seconds) ? CLOCK_24H_SEC_FMT : CLOCK_24H_FMT;
    else
        dc->cfmt = (dc->show_seconds) ? CLOCK_12H_SEC_FMT : CLOCK_12H_FMT;
    // Allocate the clock destination pixbuf sized to fit the chosen format
    dclock_create_pixbufs(dc);
    // Recolour glyphs only if the user changed the colour from the default
    if (dc->color != 0xff000000)
        dclock_set_color(dc->glyphs, dc->color);

    // Create the GtkImage backed by dc->clock; GtkImage takes a reference
    dc->main = gtk_image_new_from_pixbuf(dc->clock);
    gtk_misc_set_alignment(GTK_MISC(dc->main), 0.5, 0.5); // centre the image
    gtk_misc_set_padding(GTK_MISC(dc->main), 1, 1);        // 1px outer padding
    gtk_container_add(GTK_CONTAINER(p->pwid), dc->main);
    //gtk_widget_show(dc->clockw);  /* left as comment by original author */
    // Connect mouse click handler to the outer plugin widget (event box)
    g_signal_connect (G_OBJECT (p->pwid), "button_press_event",
            G_CALLBACK (clicked), (gpointer) dc);
    gtk_widget_show_all(dc->main);
    // Start the 1-second recurring timer; handle stored for later removal
    dc->timer = g_timeout_add(1000, (GSourceFunc) clock_update, (gpointer)dc);
    clock_update(dc); // render immediately so there is no blank frame at start

    RET(1);
}


/* Plugin class descriptor -- read by the plugin loader at dlopen() time.
 * The framework uses class_ptr to locate the descriptor. */
static plugin_class class = {
    .fname       = NULL,             /* filled in by plugin loader */
    .count       = 0,                /* instance counter          */
    .type        = "dclock",
    .name        = "Digital Clock",
    .version     = "1.0",
    .description = "Digital clock with tooltip",
    .priv_size   = sizeof(dclock_priv), /* bytes to allocate per instance */

    .constructor = dclock_constructor,
    .destructor  = dclock_destructor,
};
/* The plugin loader finds this symbol by convention */
static plugin_class *class_ptr = (plugin_class *) &class;
