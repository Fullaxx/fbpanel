/*
 * fb-background-monitor.c:
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
 *      Mark McLoughlin <mark@skynet.ie>
 */

/*
 * bg.c - FbBg: root-window background pixmap tracker for fbpanel.
 *
 * Overview
 * --------
 * FbBg is a GObject subclass (inheriting directly from GObject) that:
 *   1. Reads the _XROOTPMAP_ID X11 atom to find the current root-window
 *      background pixmap set by wallpaper daemons (e.g. feh, nitrogen).
 *   2. Maintains an X11 GC (Graphics Context) configured with FillTiled
 *      mode so that any rectangle on any drawable can be filled with a
 *      portion of the root pixmap — enabling pseudo-transparency.
 *   3. Emits a GObject "changed" signal (connected by the panel) whenever
 *      the background has been updated, so that all panel widgets can
 *      redraw themselves with the new background.
 *
 * Singleton pattern (fb_bg_get_for_display)
 * -----------------------------------------
 * The module keeps a single static pointer `default_bg` to share one FbBg
 * instance across the application.  The first caller creates the object;
 * subsequent callers receive an extra g_object_ref().  The object is
 * destroyed (and `default_bg` reset to NULL) inside fb_bg_finalize when the
 * last reference is dropped.
 *
 * Memory ownership
 * ----------------
 * - FbBg itself is a GObject; callers must g_object_unref() when done.
 * - The GdkPixmap pointers returned by fb_bg_get_xroot_pix_for_win() and
 *   fb_bg_get_xroot_pix_for_area() are caller-owned (ref count 1); the
 *   caller must g_object_unref() them.
 * - The X11 GC (bg->gc) is owned by FbBg and freed in fb_bg_finalize.
 * - bg->pixmap is an X11 Pixmap ID borrowed from the root window; it must
 *   NOT be XFreePixmap()'d by this code.
 */

#include <glib.h>
#include <glib-object.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "bg.h"
#include "panel.h"

//#define DEBUGPRN
#include "dbg.h"


/* Signal index table — only one signal ("changed") is defined. */
enum {
    CHANGED,       // emitted when the root background pixmap changes
    LAST_SIGNAL
};


/*
 * _FbBgClass - the GObject class structure for FbBg.
 *
 * Fields:
 *   parent_class - the GObjectClass that this class inherits from.
 *   changed      - default class handler for the "changed" signal.
 *                  Wired to fb_bg_changed() in fb_bg_class_init.
 *                  Overrideable by subclasses.
 */
struct _FbBgClass {
    GObjectClass   parent_class;
    void         (*changed) (FbBg *monitor);
};

/*
 * _FbBg - the GObject instance structure for FbBg.
 *
 * Fields:
 *   parent_instance - GObject header (ref-count, type info, etc.).
 *   xroot           - X11 Window ID of the root window for the default screen.
 *   id              - X11 Atom for "_XROOTPMAP_ID" (the EWMH property that
 *                     wallpaper daemons set to advertise the root pixmap).
 *   gc              - X11 GC configured with FillTiled + the root pixmap as
 *                     tile; used to blit background regions into child pixmaps.
 *   dpy             - The X11 Display connection (borrowed from GDK; do not close).
 *   pixmap          - The current root-window background X11 Pixmap ID.
 *                     Value is None (0) when no wallpaper daemon has set one.
 *                     This is a borrowed ID; do NOT call XFreePixmap on it.
 */
struct _FbBg {
    GObject    parent_instance;

    Window   xroot;
    Atom     id;
    GC       gc;
    Display *dpy;
    Pixmap   pixmap;
};

/* Forward declarations for static (internal) functions. */
static void fb_bg_class_init (FbBgClass *klass);
static void fb_bg_init (FbBg *monitor);
static void fb_bg_finalize (GObject *object);
static void fb_bg_changed(FbBg *monitor);
static Pixmap fb_bg_get_xrootpmap_real(FbBg *bg);

/* Array holding the signal IDs registered by g_signal_new().
 * Indexed by the CHANGED enum value above. */
static guint signals [LAST_SIGNAL] = { 0 };

/*
 * default_bg - module-level singleton pointer.
 * Set to non-NULL by fb_bg_get_for_display() on first call.
 * Reset to NULL inside fb_bg_finalize() when the object is destroyed.
 *
 * NOTE: This pointer is never g_object_ref()'d by the module itself;
 * it is a weak back-reference.  fb_bg_finalize() clears it so that
 * a new instance can be created if needed.
 */
static FbBg *default_bg = NULL;

/*
 * fb_bg_get_type - GObject type registration for FbBg.
 *
 * Returns: the GType ID for FbBg, registering it on the first call.
 *
 * This is the standard GObject type-system boilerplate.  The type is
 * registered as a plain GObject (G_TYPE_OBJECT) with no special flags.
 * Thread-safety: NOT thread-safe (no locking around the static variable).
 * In practice fbpanel runs single-threaded so this is acceptable.
 */
GType
fb_bg_get_type (void)
{
    static GType object_type = 0;

    if (!object_type) {
        static const GTypeInfo object_info = {
            sizeof (FbBgClass),
            (GBaseInitFunc) NULL,
            (GBaseFinalizeFunc) NULL,
            (GClassInitFunc) fb_bg_class_init,
            NULL,           /* class_finalize */
            NULL,           /* class_data */
            sizeof (FbBg),
            0,              /* n_preallocs */
            (GInstanceInitFunc) fb_bg_init,
        };

        object_type = g_type_register_static (
            G_TYPE_OBJECT, "FbBg", &object_info, 0);
    }

    return object_type;
}



/*
 * fb_bg_class_init - GObject class initialiser for FbBgClass.
 *
 * Parameters:
 *   klass - the FbBgClass being initialised (allocated by GObject type system).
 *
 * Responsibilities:
 *   1. Registers the "changed" GObject signal.  The signal takes no arguments
 *      and returns void.  G_SIGNAL_RUN_FIRST means the default class handler
 *      (fb_bg_changed) runs before any connected user callbacks.
 *   2. Points the class vtable's changed slot at fb_bg_changed so that
 *      emitting the signal always refreshes the cached pixmap.
 *   3. Overrides GObjectClass::finalize with fb_bg_finalize to perform
 *      X11 resource cleanup.
 */
static void
fb_bg_class_init (FbBgClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    ENTER;
    signals [CHANGED] =
        g_signal_new ("changed",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,           // class handler runs first
              G_STRUCT_OFFSET (FbBgClass, changed),  // vtable slot offset
              NULL, NULL,                   // no accumulator
              g_cclosure_marshal_VOID__VOID,// signal passes no args
              G_TYPE_NONE, 0);              // returns void, 0 extra params
    klass->changed = fb_bg_changed;         // wire default handler
    object_class->finalize = fb_bg_finalize;// override finalize for cleanup
    RET();
}

/*
 * fb_bg_init - GObject instance initialiser for FbBg.
 *
 * Parameters:
 *   bg - the newly allocated FbBg instance (zeroed by GObject machinery).
 *
 * Called automatically by g_object_new() immediately after memory allocation.
 *
 * Responsibilities:
 *   1. Obtains the X11 Display and root window from GDK.
 *   2. Interns the "_XROOTPMAP_ID" atom (creates it if not yet present on
 *      the X server, because False is passed for only_if_exists).
 *   3. Reads the current root pixmap with fb_bg_get_xrootpmap_real().
 *   4. Creates an X11 GC with FillTiled fill style and, if a pixmap exists,
 *      sets the tile to that pixmap.  The tile origin is set to (0,0)
 *      initially; it is adjusted per-window in the drawing functions.
 *
 * NOTE: XInternAtom is called with only_if_exists=False, so the atom is
 * created even if no wallpaper daemon has set _XROOTPMAP_ID yet.  This is
 * correct behaviour — the atom just won't have a value until a daemon sets it.
 */
static void
fb_bg_init (FbBg *bg)
{
    XGCValues  gcv;
    uint mask;

    ENTER;
    bg->dpy = GDK_DISPLAY();                      // borrow GDK's Display; do NOT close it
    bg->xroot = DefaultRootWindow(bg->dpy);        // root window of default screen
    bg->id = XInternAtom(bg->dpy, "_XROOTPMAP_ID", False); // intern atom, creating if absent
    bg->pixmap = fb_bg_get_xrootpmap_real(bg);     // read current root pixmap (may be None)

    // Set up GC parameters for FillTiled blitting
    gcv.ts_x_origin = 0;         // tile origin X (adjusted later per widget)
    gcv.ts_y_origin = 0;         // tile origin Y (adjusted later per widget)
    gcv.fill_style = FillTiled;  // fill operations use the tile pixmap
    mask = GCTileStipXOrigin | GCTileStipYOrigin | GCFillStyle;

    if (bg->pixmap != None) {
        gcv.tile = bg->pixmap;   // set the tile to the current root pixmap
        mask |= GCTile ;         // include GCTile in the mask so XCreateGC sets it
    }
    // Create GC on the root window; it can be used to draw on any same-depth drawable
    bg->gc = XCreateGC (bg->dpy, bg->xroot, mask, &gcv) ;
    RET();
}


/*
 * fb_bg_new - allocate and return a new FbBg instance.
 *
 * Returns: a newly created FbBg with reference count 1.
 *          Caller must eventually call g_object_unref().
 *
 * In normal usage, prefer fb_bg_get_for_display() to get the shared singleton.
 */
FbBg *
fb_bg_new()
{
    ENTER;
    RET(g_object_new (FB_TYPE_BG, NULL));
}

/*
 * fb_bg_finalize - GObject finalize override; cleans up X11 resources.
 *
 * Parameters:
 *   object - the GObject being destroyed (cast to FbBg internally).
 *
 * Called by the GObject machinery when the reference count reaches zero.
 * This is the correct place for resource cleanup in GObject.
 *
 * Responsibilities:
 *   1. Frees the X11 GC created in fb_bg_init().
 *   2. Clears the module-level singleton pointer `default_bg` so that
 *      fb_bg_get_for_display() can create a fresh instance if called again.
 *
 * NOTE: The parent GObjectClass finalize is NOT chained here (no call to
 * G_OBJECT_CLASS(parent_class)->finalize(object)).  For a direct GObject
 * subclass this is technically harmless since GObject's own finalize is
 * a no-op, but it is still bad practice and would be a bug for deeper
 * inheritance chains.
 *
 * NOTE: bg->pixmap is a borrowed X11 Pixmap ID owned by the root window /
 * wallpaper daemon.  It must NOT be XFreePixmap()'d here.
 */
static void
fb_bg_finalize (GObject *object)
{
    FbBg *bg;

    ENTER;
    bg = FB_BG (object);
    XFreeGC(bg->dpy, bg->gc);  // release the X11 GC; bg->dpy is still valid here
    default_bg = NULL;          // clear singleton so a new one can be created later

    RET();
}

/*
 * fb_bg_get_xrootpmap - return the cached root pixmap ID.
 *
 * Parameters:
 *   bg - a valid FbBg instance (must not be NULL).
 *
 * Returns: the X11 Pixmap ID of the current root background pixmap.
 *          Returns None (0) if no wallpaper daemon has set _XROOTPMAP_ID.
 *
 * This is a simple accessor; the value is cached from the last call to
 * fb_bg_get_xrootpmap_real() (i.e., at init time or on "changed" signal).
 * Caller must NOT XFreePixmap() the returned ID — it is owned by the
 * wallpaper daemon / X server.
 */
Pixmap
fb_bg_get_xrootpmap(FbBg *bg)
{
    ENTER;
    RET(bg->pixmap);
}

/*
 * fb_bg_get_xrootpmap_real - query the X server for the current root pixmap.
 *
 * Parameters:
 *   bg - a valid FbBg instance.
 *
 * Returns: the X11 Pixmap ID read from the "_XROOTPMAP_ID" property on the
 *          root window, or None (0) if the property is absent or unreadable.
 *
 * This function calls XGetWindowProperty() to read the _XROOTPMAP_ID atom
 * from the root window.  The retry loop (do { } while (--c > 0)) attempts
 * the read up to 2 times in case of transient X errors, although Xlib will
 * generally succeed or fail deterministically without retries.
 *
 * Memory: XGetWindowProperty allocates `prop` via XMalloc; it is freed with
 * XFree() immediately after the Pixmap ID has been copied out.
 *
 * NOTE: The double-attempt loop is unusual.  The variable `c` starts at 2
 * and the loop exits early by setting c = -c (making it negative) once a
 * valid XA_PIXMAP result is found.  If XGetWindowProperty fails or returns
 * a non-Pixmap type, c is decremented normally and the loop retries once.
 * This means the function may attempt the property read twice on failure,
 * which is unlikely to help since X property reads are not transient.
 */
static Pixmap
fb_bg_get_xrootpmap_real(FbBg *bg)
{
    Pixmap ret = None;  // default: no background pixmap

    ENTER;
    if (bg->id)  // only proceed if the atom was successfully interned
    {
        int  act_format, c = 2 ; // c: retry counter (max 2 attempts)
        u_long  nitems ;          // number of items returned by XGetWindowProperty
        u_long  bytes_after ;     // bytes remaining (0 means we got all data)
        u_char *prop = NULL;      // raw property data; must be XFree()'d
        Atom ret_type;            // actual type of the property returned

        do
        {
            if (XGetWindowProperty(bg->dpy, bg->xroot, bg->id, 0, 1,
                    False,          // do not delete property after reading
                    XA_PIXMAP,      // expected type
                    &ret_type, &act_format,
                    &nitems, &bytes_after, &prop) == Success)
            {
                if (ret_type == XA_PIXMAP)  // property exists and has correct type
                {
                    ret = *((Pixmap *)prop); // copy the Pixmap ID out of the raw buffer
                    c = -c ; //to quit loop  // set c negative to break loop early
                }
                XFree(prop);  // always free the buffer allocated by XGetWindowProperty
            }
        } while (--c > 0);  // retry up to once more on failure
    }
    RET(ret);

}



/*
 * fb_bg_get_xroot_pix_for_area - create a GdkPixmap containing the root
 *                                background for an arbitrary screen region.
 *
 * Parameters:
 *   bg     - a valid FbBg instance; must have a non-None pixmap.
 *   x      - X coordinate of the region on the root window (screen coords).
 *   y      - Y coordinate of the region on the root window (screen coords).
 *   width  - width of the region in pixels (must be > 0).
 *   height - height of the region in pixels (must be > 0).
 *   depth  - colour depth for the new GdkPixmap (should match the screen depth).
 *
 * Returns: a newly allocated GdkPixmap (ref count 1) containing the root
 *          background cropped to the specified rectangle, or NULL on failure.
 *          Caller owns the returned GdkPixmap and must g_object_unref() it.
 *
 * How it works:
 *   1. Allocates a new server-side GdkPixmap of size width x height.
 *   2. Sets the GC tile origin to (-x, -y) so that the root pixmap tile
 *      aligns correctly with the target rectangle's position on screen.
 *   3. Uses XFillRectangle with the FillTiled GC to blit the relevant
 *      portion of the root background into the new pixmap.
 *
 * NOTE: gdk_pixmap_new(NULL, ...) creates a pixmap on the default screen.
 * The `depth` parameter must match the root pixmap depth or X will throw a
 * BadMatch error during XFillRectangle.  The caller is responsible for
 * passing the correct depth.
 *
 * NOTE: No validation is done on width/height being > 0.  Passing 0 for
 * either dimension to gdk_pixmap_new will likely cause an X BadValue error.
 */
GdkPixmap *
fb_bg_get_xroot_pix_for_area(FbBg *bg, gint x, gint y,
    gint width, gint height, gint depth)
{
    GdkPixmap *gbgpix;  // the new GdkPixmap to return (caller must unref)
    Pixmap bgpix;       // the underlying X11 Pixmap ID of gbgpix

    ENTER;
    if (bg->pixmap == None)   // no root pixmap set — pseudo-transparency impossible
        RET(NULL);
    gbgpix = gdk_pixmap_new(NULL, width, height, depth);  // allocate server-side pixmap
    if (!gbgpix) {
        ERR("gdk_pixmap_new failed\n");
        RET(NULL);
    }
    bgpix =  gdk_x11_drawable_get_xid(gbgpix);  // get the underlying X11 Pixmap ID
    XSetTSOrigin(bg->dpy, bg->gc, -x, -y) ;     // offset tile origin so root coords map correctly
    XFillRectangle(bg->dpy, bgpix, bg->gc, 0, 0, width, height); // blit root bg into new pixmap
    RET(gbgpix);
}

/*
 * fb_bg_get_xroot_pix_for_win - create a GdkPixmap with the root background
 *                               portion that lies behind a given GtkWidget.
 *
 * Parameters:
 *   bg     - a valid FbBg instance; must have a non-None pixmap.
 *   widget - the GtkWidget whose position on screen determines the crop.
 *            The widget must be realised (widget->window must be non-NULL)
 *            for GDK_WINDOW_XWINDOW() and XGetGeometry() to succeed.
 *
 * Returns: a newly allocated GdkPixmap (ref count 1) containing the root
 *          background behind the widget, or NULL on any failure.
 *          Caller owns the returned GdkPixmap and must g_object_unref() it.
 *
 * How it works:
 *   1. Calls XGetGeometry to retrieve the widget window's size and depth
 *      (uses x,y from XGetGeometry only as a scratch; they are relative to
 *      the parent window, not the root, so they are discarded).
 *   2. Skips windows that are 1x1 or smaller (likely unmapped/iconified).
 *   3. Calls XTranslateCoordinates to get the widget's true position
 *      relative to the root window.
 *   4. Creates a new GdkPixmap and blits the background into it using the
 *      same FillTiled technique as fb_bg_get_xroot_pix_for_area().
 *
 * NOTE: The depth passed to gdk_pixmap_new comes from XGetGeometry and
 * represents the widget window's actual bit depth.  This should match the
 * root pixmap depth in most configurations, but on multi-depth displays it
 * might not, causing an X BadMatch error.
 *
 * NOTE: Skipping windows with width <= 1 || height <= 1 is a guard against
 * zero-size or initialising windows, but the boundary is >1 not >=1, meaning
 * a 1-pixel-wide widget would be silently skipped.
 */
GdkPixmap *
fb_bg_get_xroot_pix_for_win(FbBg *bg, GtkWidget *widget)
{
    Window win;         // X11 window ID for the widget
    Window dummy;       // scratch window (returned by XGetGeometry / XTranslateCoordinates)
    Pixmap bgpix;       // underlying X11 Pixmap ID of the new GdkPixmap
    GdkPixmap *gbgpix;  // the new GdkPixmap to return (caller must unref)
    guint  width, height, border, depth; // geometry of the widget window
    int  x, y;          // screen coordinates of the widget window top-left

    ENTER;
    if (bg->pixmap == None)   // no root background pixmap — cannot do pseudo-transparency
        RET(NULL);

    win = GDK_WINDOW_XWINDOW(widget->window);  // get raw X11 Window ID from GDK
    if (!XGetGeometry(bg->dpy, win, &dummy, &x, &y, &width, &height, &border,
              &depth)) {        // retrieve size and depth; x,y here are parent-relative
        DBG2("XGetGeometry failed\n");
        RET(NULL);
    }
    if (width <= 1 || height <= 1)  // skip tiny/unmapped windows (no useful background area)
        RET(NULL);

    // Translate the widget's top-left corner to root-window coordinates
    XTranslateCoordinates(bg->dpy, win, bg->xroot, 0, 0, &x, &y, &dummy);
    DBG("win=%lx %dx%d%+d%+d\n", win, width, height, x, y);
    gbgpix = gdk_pixmap_new(NULL, width, height, depth);  // allocate server-side pixmap
    if (!gbgpix) {
        ERR("gdk_pixmap_new failed\n");
        RET(NULL);
    }
    bgpix =  gdk_x11_drawable_get_xid(gbgpix);  // get underlying X11 Pixmap ID
    XSetTSOrigin(bg->dpy, bg->gc, -x, -y) ;     // offset tile so root coords align
    XFillRectangle(bg->dpy, bgpix, bg->gc, 0, 0, width, height); // blit root bg
    RET(gbgpix);
}

/*
 * fb_bg_composite - composite a tint colour onto a GdkDrawable in-place.
 *
 * Parameters:
 *   base      - the GdkDrawable (usually a GdkPixmap) to tint.  It must
 *               already contain the background image to be tinted.
 *               The drawable is modified in-place.
 *   gc        - a GdkGC used to draw the result back onto `base`.
 *   tintcolor - 32-bit ARGB/RGBA colour used as the tint source and
 *               checkerboard colours in gdk_pixbuf_composite_color_simple().
 *               The exact byte ordering depends on the pixbuf format.
 *   alpha     - opacity of the *background*, in the range [0..255].
 *               The tint opacity is (255 - alpha), so alpha=0 means fully
 *               tinted (no background visible); alpha=255 means no tint.
 *
 * Returns: void.  Modifies `base` in-place.
 *
 * How it works:
 *   1. Reads the current contents of `base` into a GdkPixbuf (ret).
 *   2. Composites the tint colour over the pixbuf using
 *      gdk_pixbuf_composite_color_simple(), producing a new pixbuf (ret2).
 *   3. Draws ret2 back onto `base` with gdk_draw_pixbuf().
 *   4. Unrefs both temporary pixbufs.
 *
 * Memory:
 *   ret  (gdk_pixbuf_get_from_drawable) — caller-allocated; unref'd here.
 *   ret2 (gdk_pixbuf_composite_color_simple) — caller-allocated; unref'd here.
 *   Neither is leaked on the normal path.
 *
 * NOTE: `cmap` is a static local pointer initialised once.  This is safe in
 * single-threaded code but would be a data race in multi-threaded code.
 *
 * NOTE: The checkerboard size passed to gdk_pixbuf_composite_color_simple is
 * MIN(w, h), which is an unusual choice; normally a small fixed value (e.g. 8)
 * is used.  This does not cause a crash but the visual result for very
 * non-square drawables may be unexpected.
 *
 * NOTE: The old gdk_pixbuf_render_to_drawable() call has been commented out
 * and replaced with gdk_draw_pixbuf().  The commented line is dead code.
 */
void
fb_bg_composite(GdkDrawable *base, GdkGC *gc, guint32 tintcolor, gint alpha)
{
    GdkPixbuf *ret, *ret2;  // temporary pixbufs; both unref'd before return
    int w, h;               // dimensions of the drawable
    static GdkColormap *cmap = NULL;  // lazily initialised system colormap

    ENTER;
    gdk_drawable_get_size (base, &w, &h);  // retrieve drawable dimensions
    if (!cmap) {
        cmap = gdk_colormap_get_system ();  // initialise once; not unref'd (intentional leak)
    }
    DBG("here\n");
    // Read back the current drawable contents as a pixbuf; cmap needed for RGB conversion
    ret = gdk_pixbuf_get_from_drawable (NULL, base, cmap, 0, 0, 0, 0, w, h);
    if (!ret)   // drawable read failed (e.g. off-screen or invalid)
        RET();
    DBG("here w=%d h=%d\n", w, h);
    // Composite the tint colour over the pixbuf at opacity (255 - alpha)
    // check_size = MIN(w,h) is unusual but harmless
    ret2 = gdk_pixbuf_composite_color_simple(ret, w, h,
          GDK_INTERP_HYPER, 255-alpha, MIN(w, h), tintcolor, tintcolor);
    DBG("here\n");
    if (!ret2) {
        g_object_unref(ret);  // avoid leak if composite step fails
        RET();
    }
    //gdk_pixbuf_render_to_drawable (ret2, base, gc, 0, 0, 0, 0, w, h, GDK_RGB_DITHER_NONE, 0, 0);
    // Draw the composited pixbuf back onto the base drawable
    gdk_draw_pixbuf (base, gc, ret2, 0, 0, 0, 0, w, h,
        GDK_RGB_DITHER_NONE, 0, 0);
    g_object_unref(ret);   // release the source pixbuf
    g_object_unref(ret2);  // release the composited pixbuf
    RET();
}


/*
 * fb_bg_changed - default class handler for the "changed" signal.
 *
 * Parameters:
 *   bg - the FbBg instance that received the signal.
 *
 * Called automatically (as the G_SIGNAL_RUN_FIRST class handler) whenever
 * g_signal_emit(bg, signals[CHANGED], 0) is called via fb_bg_notify_changed_bg().
 *
 * Responsibilities:
 *   1. Re-reads the _XROOTPMAP_ID property to get the new root pixmap ID.
 *   2. If a valid pixmap is found, updates the GC tile so subsequent
 *      XFillRectangle calls use the new background.
 *
 * After this handler completes, any user-connected "changed" callbacks fire
 * (because G_SIGNAL_RUN_FIRST means class handler goes first).
 *
 * NOTE: If the new pixmap is None (wallpaper removed), bg->pixmap is set to
 * None but the GC tile is NOT updated.  The GC still references the old
 * pixmap tile.  This means fb_bg_get_xroot_pix_for_win / _for_area will
 * correctly return NULL (due to the None check), but the GC is left in an
 * inconsistent state pointing to a potentially freed X pixmap.
 */
static void
fb_bg_changed(FbBg *bg)
{
    ENTER;
    bg->pixmap = fb_bg_get_xrootpmap_real(bg);  // re-read root pixmap from X server
    if (bg->pixmap != None) {
        XGCValues  gcv;

        gcv.tile = bg->pixmap;                       // set new tile in GC values struct
        XChangeGC(bg->dpy, bg->gc, GCTile, &gcv);   // update only the tile field of the GC
        DBG("changed\n");
    }
    RET();
}


/*
 * fb_bg_notify_changed_bg - emit the "changed" signal on an FbBg instance.
 *
 * Parameters:
 *   bg - the FbBg instance to notify.
 *
 * This is the external API for notifying the background monitor that the
 * root pixmap has changed (e.g. called from the panel's X event handler
 * when a PropertyNotify event for _XROOTPMAP_ID is received on the root).
 *
 * The signal emission sequence:
 *   1. fb_bg_changed() runs (G_SIGNAL_RUN_FIRST class handler) — updates
 *      bg->pixmap and the GC tile.
 *   2. All connected user callbacks run — panel widgets redraw themselves.
 */
void fb_bg_notify_changed_bg(FbBg *bg)
{
    ENTER;
    g_signal_emit (bg, signals [CHANGED], 0);  // trigger "changed" signal, no extra args
    RET();
}

/*
 * fb_bg_get_for_display - return the process-wide shared FbBg singleton.
 *
 * Returns: an FbBg instance with its reference count incremented.
 *          Caller must g_object_unref() it when done.
 *
 * Singleton semantics:
 *   - First call: creates a new FbBg (ref count 1 from g_object_new) and
 *     stores the pointer in `default_bg`.  Returns it with ref count 1.
 *   - Subsequent calls: calls g_object_ref(default_bg) (bumping to 2, 3, …)
 *     and returns the same pointer.
 *
 * NOTE: On first call, the returned ref count is 1 (no extra g_object_ref).
 * On subsequent calls, g_object_ref is called, making the ref count 2+ for
 * the first caller.  This is ASYMMETRIC: the first caller holds a ref count
 * of 1 while later callers each hold an additional ref.  When fb_bg_finalize
 * clears default_bg, the next call to fb_bg_get_for_display() creates a
 * fresh instance.  This design is intentional for a shared singleton but
 * requires every caller (including the first) to call g_object_unref().
 *
 * NOTE: There is a ref-counting inconsistency on the first call: `default_bg`
 * is set to the raw g_object_new() result (ref count=1) WITHOUT calling
 * g_object_ref(), yet fb_bg_finalize sets default_bg=NULL.  If any caller
 * holds a ref and calls g_object_unref(), it destroys the singleton while
 * `default_bg` might still be non-NULL from a previous cycle — but since
 * finalize clears it, this is safe.  However, the first caller gets ref=1
 * while all others get their own extra ref — so the lifetime of the singleton
 * depends entirely on the first caller not dropping its reference prematurely.
 */
FbBg *fb_bg_get_for_display(void)
{
    ENTER;
    if (!default_bg)
        default_bg = fb_bg_new();  // create singleton; ref count = 1; no extra ref here
    else
        g_object_ref(default_bg);  // bump ref count for this additional caller
    RET(default_bg);
}
