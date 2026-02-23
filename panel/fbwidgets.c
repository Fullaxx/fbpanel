/*
 * fbwidgets.c -- GTK widget factory for fbpanel.
 *
 * Implements the GtkIconTheme singleton, color utilities, and the
 * fb_pixbuf / fb_image / fb_button composite widget family.
 *
 * Extracted from misc.c.  No logic changes.
 *
 * Widget ownership conventions:
 *   - Functions whose names end with "_new" return a newly created widget
 *     with ref-count 1; the caller should add it to a container (which
 *     transfers ownership) or manage the ref explicitly.
 *   - GdkPixbuf objects returned by fb_pixbuf_new() are owned by the caller
 *     and must be released with g_object_unref().
 */

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "fbwidgets.h"
#include "gtkbgbox.h"

//#define DEBUGPRN
#include "dbg.h"

/* Default GTK icon theme, obtained once in fb_init() (misc.c).
 * Not ref-counted; GtkIconTheme is a singleton managed by GTK itself. */
GtkIconTheme *icon_theme;


/* ---------------------------------------------------------------------------
 * Button spacing / color utilities
 * --------------------------------------------------------------------------- */

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


/* ---------------------------------------------------------------------------
 * FB Pixbuf
 * --------------------------------------------------------------------------- */

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


/* ---------------------------------------------------------------------------
 * FB Image
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
 * --------------------------------------------------------------------------- */

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


/* ---------------------------------------------------------------------------
 * FB Button
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
 * --------------------------------------------------------------------------- */

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
