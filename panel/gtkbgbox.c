/* GTK - The GIMP Toolkit
 * Copyright (C) 1995-1997 Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Modified by the GTK+ Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GTK+ Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GTK+ at ftp://ftp.gtk.org/pub/gtk/.
 */

/*
 * gtkbgbox.c -- Implementation of GtkBgbox.
 *
 * GtkBgbox is a single-child container (GtkBin subclass) that owns its own
 * X/GDK window and supports three background rendering strategies:
 *
 *   BG_STYLE   : delegate to the GTK theme (gtk_style_set_background).
 *   BG_ROOT    : copy the region of the X root-window pixmap that lies
 *                beneath this widget, optionally applying a colour tint via
 *                fb_bg_composite(), then set the copy as the window background
 *                pixmap.  This creates the "pseudo-transparent" effect.
 *   BG_INHERIT : set the GDK background to NULL with parent-relative TRUE,
 *                so X will expose the parent window's backing store through
 *                this window (cheap "inherit parent" transparency).
 *
 * Background updates are triggered automatically by:
 *   - Initial realisation (gtk_bgbox_realize).
 *   - GTK style changes (gtk_bgbox_style_set).
 *   - Size/position changes (gtk_bgbox_size_allocate).
 *   - Root pixmap changes signalled by FbBg (gtk_bgbox_bg_changed).
 *   - ConfigureNotify events on the widget's own window (gtk_bgbox_event_filter).
 *
 * Private state:
 *   All mutable state is kept in GtkBgboxPrivate, allocated inline by the
 *   GObject machinery.  The struct is accessed via gtk_bgbox_get_instance_private().
 *
 * Ref-count / ownership rules:
 *   priv->bg     : one ref held on the FbBg singleton (fb_bg_get_for_display).
 *                  Acquired lazily in gtk_bgbox_set_background when a root/inherit
 *                  mode is first requested.  Released in gtk_bgbox_finalize.
 *   priv->sid    : GLib signal handler ID for the FbBg "changed" signal.
 *                  Must be disconnected before priv->bg is unref'd; failing to
 *                  do so in the right order would leave a dangling handler that
 *                  could fire on a dead widget.
 *   priv->pixmap : GdkPixmap owned by GtkBgbox.  Allocated by
 *                  fb_bg_get_xroot_pix_for_win() which returns a new reference.
 *                  Freed (g_object_unref) before each replacement and in finalize.
 */

#include <string.h>
#include "gtkbgbox.h"
#include "bg.h"
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkpixmap.h>
#include <gdk/gdkprivate.h>  // needed for GDK_NO_BG sentinel (private GDK API)
#include <glib.h>
#include <glib-object.h>


//#define DEBUGPRN
#include "dbg.h"  // provides ENTER/RET/DBG/ERR tracing macros

/*
 * GtkBgboxPrivate -- per-instance private data.
 *
 * Allocated inline after the GtkBgbox struct by G_ADD_PRIVATE(); access via
 * gtk_bgbox_get_instance_private(instance).
 *
 * Fields:
 *   pixmap    - GdkPixmap sampled from the root window (BG_ROOT mode only).
 *               NULL when not in BG_ROOT mode or when sampling failed.
 *               Owned (one ref held); freed before replacement and in finalize.
 *   tintcolor - 24-bit RGB tint colour (top byte ignored) used with BG_ROOT.
 *   alpha     - tint strength [0..255].  0 = no tint; 255 = fully opaque tint.
 *   bg_type   - current background mode; one of the BG_* enum values.
 *   bg        - the FbBg singleton for the default display.
 *               NULL when BG_STYLE mode is active (no root pixmap monitoring
 *               needed).  One ref held when non-NULL; released in finalize.
 *   sid       - GLib signal handler ID returned by g_signal_connect() for
 *               the FbBg "changed" signal.  0 when disconnected.
 */
typedef struct {
    GdkPixmap *pixmap;
    guint32 tintcolor;
    gint alpha;
    int bg_type;
    FbBg *bg;
    gulong sid;
} GtkBgboxPrivate;

/*
 * G_DEFINE_TYPE_WITH_CODE registers GtkBgbox as a GObject subtype of
 * GTK_TYPE_BIN and calls G_ADD_PRIVATE() to reserve private-data storage
 * sized to GtkBgboxPrivate.  This also generates the gtk_bgbox_get_type()
 * function body and the gtk_bgbox_get_instance_private() accessor.
 */
G_DEFINE_TYPE_WITH_CODE(GtkBgbox, gtk_bgbox, GTK_TYPE_BIN, G_ADD_PRIVATE(GtkBgbox))

/* Forward declarations of static functions implemented below. */
static void gtk_bgbox_class_init    (GtkBgboxClass *klass);
static void gtk_bgbox_init          (GtkBgbox *bgbox);
static void gtk_bgbox_realize       (GtkWidget *widget);
static void gtk_bgbox_size_request  (GtkWidget *widget, GtkRequisition   *requisition);
static void gtk_bgbox_size_allocate (GtkWidget *widget, GtkAllocation    *allocation);
static void gtk_bgbox_style_set (GtkWidget *widget, GtkStyle  *previous_style);
static gboolean gtk_bgbox_configure_event(GtkWidget *widget, GdkEventConfigure *e);
#if 0
/* These handlers are stubbed out; GTK's default destroy/delete handling is
 * sufficient for GtkBgbox and the implementations were removed. */
static gboolean gtk_bgbox_destroy_event (GtkWidget *widget, GdkEventAny *event);
static gboolean gtk_bgbox_delete_event (GtkWidget *widget, GdkEventAny *event);
#endif

static void gtk_bgbox_finalize (GObject *object);

/* Private helpers; only called from within this file. */
static void gtk_bgbox_set_bg_root(GtkWidget *widget, GtkBgboxPrivate *priv);
static void gtk_bgbox_set_bg_inherit(GtkWidget *widget, GtkBgboxPrivate *priv);
static void gtk_bgbox_bg_changed(FbBg *bg, GtkWidget *widget);

/*
 * parent_class -- cached pointer to the GtkBinClass vtable.
 *
 * Populated in gtk_bgbox_class_init via g_type_class_peek_parent().
 * Used to chain up to parent-class vfunc implementations.  In modern GTK2
 * practice this is often replaced with G_OBJECT_CLASS / GTK_WIDGET_CLASS
 * macros, but the pattern here is a common older idiom.
 */
static GtkBinClass *parent_class = NULL;

/*
 * gtk_bgbox_class_init:
 *
 * Called once by the GObject type system when the GtkBgbox GType is first
 * registered.  Sets up the class vtable by overriding selected parent-class
 * virtual functions with GtkBgbox-specific implementations.
 *
 * Parameters:
 *   class - the freshly allocated GtkBgboxClass struct to populate.
 *
 * No return value.
 *
 * Note: GObject automatically chains finalize up the class hierarchy;
 * however we must explicitly chain vfuncs like realize/size_request if we
 * want parent behaviour (here we intentionally replace them wholesale).
 */
static void
gtk_bgbox_class_init (GtkBgboxClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS (class);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

    // Save the parent class pointer so we can chain up from instance methods.
    parent_class = g_type_class_peek_parent (class);

    // Override widget virtual functions with our custom implementations.
    widget_class->realize         = gtk_bgbox_realize;        // custom window creation
    widget_class->size_request    = gtk_bgbox_size_request;   // propagate to child
    widget_class->size_allocate   = gtk_bgbox_size_allocate;  // move/resize + bg update
    widget_class->style_set       = gtk_bgbox_style_set;      // re-apply bg on theme change
    widget_class->configure_event = gtk_bgbox_configure_event;// currently a no-op stub

    // Override finalize to release pixmap and FbBg resources.
    object_class->finalize = gtk_bgbox_finalize;
}

/*
 * gtk_bgbox_init:
 *
 * GObject instance initialiser; called for each newly allocated GtkBgbox
 * before any other code sees the instance.
 *
 * Parameters:
 *   bgbox - the newly created (but not yet realised) GtkBgbox instance.
 *
 * Side effects:
 *   - Clears the GTK_NO_WINDOW flag so the widget creates its own GdkWindow.
 *     (GtkBin sets GTK_NO_WINDOW by default in some GTK versions; clearing it
 *     here ensures we get an independent drawable for background rendering.)
 *   - Sets priv->bg_type to BG_NONE (no background configured yet).
 *   - Sets priv->sid to 0 (no signal handler connected yet).
 *   - All other GtkBgboxPrivate fields are zero-initialised by GObject.
 */
static void
gtk_bgbox_init (GtkBgbox *bgbox)
{
    GtkBgboxPrivate *priv;

    ENTER;
    // Ensure we get an input/output window; required for gdk_window_set_back_pixmap.
    GTK_WIDGET_UNSET_FLAGS (bgbox, GTK_NO_WINDOW);

    priv = gtk_bgbox_get_instance_private(bgbox);
    priv->bg_type = BG_NONE;  // no mode selected yet; will become BG_STYLE at realize
    priv->sid = 0;             // no FbBg "changed" signal connected yet
    RET();
}

/*
 * gtk_bgbox_new:
 *
 * Public constructor.  Allocates a GtkBgbox via the GObject system with no
 * construction parameters.
 *
 * Returns: a GtkWidget* with a floating reference.  Ownership passes to the
 *          parent container when the widget is packed via gtk_box_pack_start()
 *          or similar.
 */
GtkWidget*
gtk_bgbox_new (void)
{
    ENTER;
    RET(g_object_new (GTK_TYPE_BGBOX, NULL));
}

/*
 * gtk_bgbox_finalize:
 *
 * GObject finalize vfunc; called when the last reference to the GtkBgbox is
 * dropped (i.e., reference count reaches zero).  This is the correct place to
 * release resources that are NOT GtkWidget-level (those go in ::destroy).
 *
 * Parameters:
 *   object - the GObject being finalised; safe to cast to GtkBgbox*.
 *
 * Resource release order matters here:
 *   1. Release priv->pixmap first (it does not depend on priv->bg).
 *   2. Disconnect the signal handler (priv->sid) BEFORE unref'ing priv->bg,
 *      because the handler holds a pointer to the widget.  Disconnecting first
 *      prevents a potential use-after-free if FbBg emitted "changed" during
 *      destruction (unlikely but possible in edge cases).
 *   3. Unref priv->bg last.
 *
 * Note: the parent class finalize is NOT explicitly chained here.  This is a
 * latent bug -- GObject requires that finalize chains always call
 * G_OBJECT_CLASS(parent_class)->finalize(object) to allow the parent class to
 * release its own resources.
 */
static void
gtk_bgbox_finalize (GObject *object)
{
    GtkBgboxPrivate *priv;

    ENTER;
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(object));

    // Step 1: release our reference to the root-window pixmap copy.
    if (priv->pixmap) {
        g_object_unref(priv->pixmap);
        priv->pixmap = NULL;  // prevent double-free
    }

    // Step 2: disconnect the FbBg "changed" signal BEFORE releasing priv->bg.
    // The handler (gtk_bgbox_bg_changed) closes over a GtkWidget*; if the
    // signal were to fire between here and the unref we'd reference freed memory.
    if (priv->sid) {
        g_signal_handler_disconnect(priv->bg, priv->sid);
        priv->sid = 0;
    }

    // Step 3: drop our reference to the FbBg singleton.
    if (priv->bg) {
        g_object_unref(priv->bg);
        priv->bg = NULL;
    }
    RET();
}

/*
 * gtk_bgbox_event_filter:
 *
 * A GDK event filter installed on this widget's own window to intercept raw
 * X events before GDK processes them.  This is necessary because GDK does not
 * forward ConfigureNotify events to child windows (GDK_WINDOW_CHILD) through
 * the normal widget signal mechanism.
 *
 * Parameters:
 *   xevent - raw XEvent from the X server (cast from GdkXEvent*).
 *   event  - the translated GdkEvent (may be incomplete/NULL for raw events).
 *   widget - user data; the GtkBgbox that installed this filter.
 *            NOTE: this is a bare GtkWidget* held without a reference.  If the
 *            widget is destroyed while the filter is still registered the
 *            pointer becomes dangling.  The filter is removed in realize (not
 *            unrealize), so there is a window where this could be a problem
 *            if the widget is unrealized but not yet finalized.
 *
 * Returns: GDK_FILTER_CONTINUE so that GDK continues processing the event
 *          through its normal pipeline even after we handle it.
 *
 * Side effects:
 *   - On ConfigureNotify: schedules a full redraw via gtk_widget_queue_draw().
 *     This re-triggers background sampling when the widget moves/resizes.
 */
static GdkFilterReturn
gtk_bgbox_event_filter(GdkXEvent *xevent, GdkEvent *event, GtkWidget *widget)
{
    XEvent *ev = (XEvent *) xevent;  // reinterpret the opaque GdkXEvent as XEvent

    ENTER;
    if (ev->type == ConfigureNotify) {
        // The widget's window moved or resized; the background must be re-sampled.
        gtk_widget_queue_draw(widget);
        DBG("ConfigureNotify %d %d %d %d\n",
              ev->xconfigure.x,
              ev->xconfigure.y,
              ev->xconfigure.width,
              ev->xconfigure.height
            );
    }
    RET(GDK_FILTER_CONTINUE);  // always let GDK continue processing
}

/*
 * gtk_bgbox_realize:
 *
 * GtkWidget::realize vfunc.  Called by GTK when the widget is first mapped to
 * a screen (i.e., when gtk_widget_show_all() or similar triggers realisation).
 * Responsible for creating the backing GdkWindow and attaching the style and
 * event filter.
 *
 * Parameters:
 *   widget - the GtkBgbox being realised.
 *
 * Side effects (in order):
 *   1. Sets GTK_REALIZED flag on the widget.
 *   2. Creates a GDK_WINDOW_CHILD window at the widget's allocated position/size.
 *   3. Associates the window with the widget (gdk_window_set_user_data) so GDK
 *      routes events back to this widget.
 *   4. Attaches the GTK style to the window.
 *   5. If no background has been configured yet (BG_NONE), initialises to
 *      BG_STYLE (theme-driven background).
 *   6. Installs gtk_bgbox_event_filter to catch raw ConfigureNotify events.
 *
 * Note: the parent class realize is NOT called, so GtkBin/GtkContainer
 * realize logic is bypassed entirely.  GtkBin::realize would normally create
 * a window via GtkWidget::realize; since we create the window ourselves, this
 * is intentional -- but it does mean we must replicate all necessary steps
 * manually (which is done here).
 *
 * Memory: widget->window is a GdkWindow owned by GTK's widget internals.
 * GDK adds a reference when gdk_window_new() is called; gtk_widget_unrealize
 * will call gdk_window_destroy which releases it.
 */
static void
gtk_bgbox_realize (GtkWidget *widget)
{
    GdkWindowAttr attributes;
    gint attributes_mask;
    gint border_width;
    GtkBgboxPrivate *priv;

    ENTER;
    GTK_WIDGET_SET_FLAGS (widget, GTK_REALIZED);  // mark widget as realised

    border_width = GTK_CONTAINER (widget)->border_width;

    // Position and size the new GDK window within the parent window.
    // The allocation is in parent-window coordinates; border_width is subtracted.
    attributes.x = widget->allocation.x + border_width;
    attributes.y = widget->allocation.y + border_width;
    attributes.width = widget->allocation.width - 2*border_width;
    attributes.height = widget->allocation.height - 2*border_width;
    attributes.window_type = GDK_WINDOW_CHILD;  // child of the panel's toplevel window

    // Request the events we need to function correctly.
    attributes.event_mask = gtk_widget_get_events (widget)
        | GDK_BUTTON_MOTION_MASK    // track drags inside the widget
        | GDK_BUTTON_PRESS_MASK     // mouse-button presses (e.g., context menu)
        | GDK_BUTTON_RELEASE_MASK   // mouse-button releases
        | GDK_ENTER_NOTIFY_MASK     // cursor entering the widget
        | GDK_LEAVE_NOTIFY_MASK     // cursor leaving the widget
        | GDK_EXPOSURE_MASK         // expose events for background repainting
        | GDK_STRUCTURE_MASK;       // configure/unmap/map events

    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));

    attributes.visual = gtk_widget_get_visual (widget);    // inherit visual from parent
    attributes.colormap = gtk_widget_get_colormap (widget); // inherit colormap
    attributes.wclass = GDK_INPUT_OUTPUT;  // full input+output window (not input-only)

    // Flags indicating which attributes fields are valid.
    attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL | GDK_WA_COLORMAP;

    // Create the GDK (X11) window.  The returned window has a refcount of 1;
    // GTK will destroy (and unref) it in gtk_widget_unrealize().
    widget->window = gdk_window_new (gtk_widget_get_parent_window (widget),
          &attributes, attributes_mask);

    // Register the widget as the handler for events on this window.
    gdk_window_set_user_data (widget->window, widget);

    // Attach the style (theme colours, GCs, etc.) to the new window.
    widget->style = gtk_style_attach (widget->style, widget->window);

    // If no background was requested before realisation, default to theme style.
    if (priv->bg_type == BG_NONE)
        gtk_bgbox_set_background(widget, BG_STYLE, 0, 0);

    // Install the raw X event filter; needed because GDK swallows ConfigureNotify
    // for child windows and does not emit a configure-event signal for them.
    gdk_window_add_filter(widget->window,  (GdkFilterFunc) gtk_bgbox_event_filter, widget);
    RET();
}


/*
 * gtk_bgbox_style_set:
 *
 * GtkWidget::style-set vfunc.  Called when the widget's GTK style changes
 * (e.g., because the user switched themes).  Re-applies the current background
 * so it reflects the new style (especially important for BG_STYLE mode).
 *
 * Parameters:
 *   widget         - the GtkBgbox whose style changed.
 *   previous_style - the old GtkStyle (may be NULL on first call); not used here.
 *
 * Guard: only acts if the widget is realised and has its own window.
 * This prevents running before the GdkWindow exists.
 */
static void
gtk_bgbox_style_set (GtkWidget *widget, GtkStyle  *previous_style)
{
    GtkBgboxPrivate *priv;

    ENTER;
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    if (GTK_WIDGET_REALIZED (widget) && !GTK_WIDGET_NO_WINDOW (widget)) {
        // Re-apply the stored background parameters against the new style.
        gtk_bgbox_set_background(widget, priv->bg_type, priv->tintcolor, priv->alpha);
    }
    RET();
}

/*
 * gtk_bgbox_configure_event:
 *
 * GtkWidget::configure-event vfunc.  Would normally be called when the widget's
 * window is moved or resized, but as noted in the comment below, GTK silently
 * drops ConfigureNotify for GDK_WINDOW_CHILD windows and this handler is never
 * actually invoked through the normal GTK signal path.
 *
 * The workaround is the raw X event filter (gtk_bgbox_event_filter) installed
 * in gtk_bgbox_realize.
 *
 * Parameters:
 *   widget - unused.
 *   e      - the configure event (position and size); logged in debug builds.
 *
 * Returns: FALSE (event not consumed; propagate normally, though it is
 *          unreachable in practice).
 */
/* gtk discards configure_event for GTK_WINDOW_CHILD. too pitty */
static  gboolean
gtk_bgbox_configure_event (GtkWidget *widget, GdkEventConfigure *e)
{
    ENTER;
    // This function body is never reached for child windows; kept as documentation.
    DBG("geom: size (%d, %d). pos (%d, %d)\n", e->width, e->height, e->x, e->y);
    RET(FALSE);

}

/*
 * gtk_bgbox_size_request:
 *
 * GtkWidget::size-request vfunc.  Calculates the widget's natural/preferred
 * size by asking its child (if visible) and adding the container border.
 *
 * Parameters:
 *   widget      - the GtkBgbox.
 *   requisition - output struct; filled in with (width, height) in pixels.
 *
 * The minimum size is 2 * border_width in each dimension.  If there is a
 * visible child, the child's own requisition is added to that baseline.
 *
 * Note: GtkBgbox does not call parent_class->size_request here; it fully
 * reimplements the GtkBin size-request logic directly.
 */
static void
gtk_bgbox_size_request (GtkWidget *widget, GtkRequisition *requisition)
{
    GtkBin *bin = GTK_BIN (widget);
    ENTER;
    // Start with the minimum size contributed purely by the container border.
    requisition->width = GTK_CONTAINER (widget)->border_width * 2;
    requisition->height = GTK_CONTAINER (widget)->border_width * 2;

    if (bin->child && GTK_WIDGET_VISIBLE (bin->child))
    {
        GtkRequisition child_requisition;

        // Recursively ask the child for its size requirements.
        gtk_widget_size_request (bin->child, &child_requisition);

        // Add the child's requirements to ours (no extra padding beyond border).
        requisition->width += child_requisition.width;
        requisition->height += child_requisition.height;
    }
    RET();
}

/*
 * gtk_bgbox_size_allocate:
 *
 * GtkWidget::size-allocate vfunc.  Called by the parent container when it
 * decides the final position and size for this widget.  Moves/resizes the
 * GDK window to match and re-applies the background so the root-pixmap sample
 * is taken from the correct screen coordinates.
 *
 * Parameters:
 *   widget - the GtkBgbox.
 *   wa     - the new allocation (x, y in parent-window coords; width, height).
 *
 * Optimisation:
 *   If the new allocation is identical to the current one (same_alloc == TRUE),
 *   the window move/resize and background re-sample are skipped.  This avoids
 *   redundant FbBg work on trivial GTK relayout passes.
 *   CAVEAT: if the root pixmap changes but the widget's allocation stays the
 *   same, the displayed background will NOT be updated through this path.
 *   Background changes are instead handled by gtk_bgbox_bg_changed (via the
 *   FbBg "changed" signal), so in practice this is safe.
 *
 * Child allocation:
 *   The child always gets (border, border, width-2*border, height-2*border).
 *   Note: child allocation uses window-local coordinates (origin 0,0) not
 *   parent-window coordinates, which is correct for a widget with its own window.
 *
 * calls with same allocation are usually refer to exactly same background
 * and we just skip them for optimization reason.
 * so if you see artifacts or unupdated background - reallocate bg on every call
 */
static void
gtk_bgbox_size_allocate (GtkWidget *widget, GtkAllocation *wa)
{
    GtkBin *bin;
    GtkAllocation ca;      // child allocation (window-local coordinates)
    GtkBgboxPrivate *priv;
    int same_alloc, border;

    ENTER;
    // Compare the incoming allocation with the current one using raw memory
    // comparison -- valid because GtkAllocation is a plain struct of ints.
    same_alloc = !memcmp(&widget->allocation, wa, sizeof(*wa));
    DBG("same alloc = %d\n", same_alloc);
    DBG("x=%d y=%d w=%d h=%d\n", wa->x, wa->y, wa->width, wa->height);
    DBG("x=%d y=%d w=%d h=%d\n", widget->allocation.x, widget->allocation.y,
          widget->allocation.width, widget->allocation.height);

    // Commit the new allocation; must happen before gdk_window_move_resize.
    widget->allocation = *wa;
    bin = GTK_BIN (widget);
    border = GTK_CONTAINER (widget)->border_width;

    // Child allocation is in widget-window-local coordinates, so x=y=border.
    ca.x = border;
    ca.y = border;
    ca.width  = MAX (wa->width  - border * 2, 0);  // clamp to 0 to avoid negative sizes
    ca.height = MAX (wa->height - border * 2, 0);

    if (GTK_WIDGET_REALIZED (widget) && !GTK_WIDGET_NO_WINDOW (widget)
          && !same_alloc) {
        priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
        DBG("move resize pos=%d,%d geom=%dx%d\n", wa->x, wa->y, wa->width,
                wa->height);
        // Move and resize the GDK window to match the new allocation.
        gdk_window_move_resize (widget->window, wa->x, wa->y, wa->width, wa->height);
        // Re-sample the background from the (potentially new) screen coordinates.
        gtk_bgbox_set_background(widget, priv->bg_type, priv->tintcolor, priv->alpha);
    }

    // Allocate space to the child widget regardless of whether we updated bg.
    if (bin->child)
        gtk_widget_size_allocate (bin->child, &ca);
    RET();
}


/*
 * gtk_bgbox_bg_changed:
 *
 * Callback invoked when the FbBg singleton emits its "changed" signal,
 * which happens whenever the X root window's background pixmap changes
 * (e.g., because a wallpaper manager updated it).
 *
 * Parameters:
 *   bg     - the FbBg object that emitted the signal (not used directly here;
 *             the priv->bg reference is used instead).
 *   widget - the GtkBgbox that registered this callback (user_data).
 *            NOTE: this is a non-owning pointer.  If the widget is destroyed
 *            while the signal is still connected the callback could receive a
 *            dangling pointer.  The signal is disconnected in gtk_bgbox_finalize,
 *            but there is a potential race if FbBg emits "changed" during
 *            widget teardown after ::destroy but before ::finalize.
 *
 * Side effects:
 *   Re-applies the stored background parameters so the new root pixmap is
 *   sampled and the widget redraws.
 */
static void
gtk_bgbox_bg_changed(FbBg *bg, GtkWidget *widget)
{
    GtkBgboxPrivate *priv;

    ENTER;
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    if (GTK_WIDGET_REALIZED (widget) && !GTK_WIDGET_NO_WINDOW (widget)) {
        // Root pixmap changed; re-sample from the new root background.
        gtk_bgbox_set_background(widget, priv->bg_type, priv->tintcolor, priv->alpha);
    }
    RET();
}

/*
 * gtk_bgbox_set_background:
 *
 * Public API.  Sets or changes the background rendering mode and parameters
 * for a GtkBgbox widget.  This is the main entry point for background
 * configuration; all internal paths funnel through this function.
 *
 * Parameters:
 *   widget    - must be a GtkBgbox (checked with GTK_IS_BGBOX; silently
 *               returns if not).
 *   bg_type   - new background mode: BG_NONE, BG_STYLE, BG_ROOT, or BG_INHERIT.
 *   tintcolor - RGB tint colour (only relevant for BG_ROOT; stored regardless).
 *   alpha     - tint opacity [0..255] (only relevant for BG_ROOT; stored
 *               regardless so it survives mode switches).
 *
 * Logic outline:
 *   1. Drop any existing pixmap reference.
 *   2. Store the new bg_type.
 *   3a. BG_STYLE: call gtk_style_set_background, disconnect/release FbBg.
 *   3b. BG_ROOT / BG_INHERIT: lazily acquire FbBg singleton, connect "changed"
 *       signal (if not already connected), then delegate to the specific helper.
 *   4. Schedule a redraw.
 *   5. Notify GObject observers that the "style" property changed.
 *
 * Ref-count side effects:
 *   - priv->pixmap: any pre-existing pixmap is unref'd at step 1.
 *   - priv->bg: if transitioning TO BG_STYLE, the FbBg ref is dropped.
 *               if transitioning FROM BG_STYLE, a new FbBg ref is acquired.
 *   - priv->sid: disconnected when transitioning to BG_STYLE.
 *
 * Note: g_object_notify(widget, "style") at the end is unusual -- "style"
 * is a GtkWidget property managed by GTK itself.  Notifying it manually may
 * cause unintended re-entrancy (GTK will call gtk_bgbox_style_set again) and
 * could lead to an infinite update loop in edge cases.
 */
void
gtk_bgbox_set_background(GtkWidget *widget, int bg_type, guint32 tintcolor, gint alpha)
{
    GtkBgboxPrivate *priv;

    ENTER;
    // Bail out if caller passed a non-GtkBgbox widget by mistake.
    if (!(GTK_IS_BGBOX (widget)))
        RET();

    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    DBG("widget=%p bg_type old:%d new:%d\n", widget, priv->bg_type, bg_type);

    // Always drop the old root-pixmap copy; a new one will be fetched if needed.
    if (priv->pixmap) {
        g_object_unref(priv->pixmap);
        priv->pixmap = NULL;
    }

    priv->bg_type = bg_type;

    if (priv->bg_type == BG_STYLE) {
        // Theme-driven background: let GTK paint it using the widget's style.
        gtk_style_set_background(widget->style, widget->window, widget->state);

        // No longer need to monitor root-pixmap changes; disconnect and release.
        if (priv->sid) {
            g_signal_handler_disconnect(priv->bg, priv->sid);
            priv->sid = 0;
        }
        if (priv->bg) {
            g_object_unref(priv->bg);
            priv->bg = NULL;
        }
    } else {
        // BG_ROOT or BG_INHERIT: we need the FbBg singleton for root-pixmap access.

        // Lazily obtain the FbBg singleton and acquire a reference.
        // fb_bg_get_for_display() returns a ref-counted singleton; we must
        // g_object_unref() it when done (done in finalize and BG_STYLE branch).
        if (!priv->bg)
            priv->bg = fb_bg_get_for_display();

        // Connect to the "changed" signal so we re-render when the wallpaper changes.
        // Only connect once; priv->sid == 0 means not yet connected.
        if (!priv->sid)
            priv->sid = g_signal_connect(G_OBJECT(priv->bg), "changed", G_CALLBACK(gtk_bgbox_bg_changed), widget);

        if (priv->bg_type == BG_ROOT) {
            // Store tint parameters for use in the helper and future redraws.
            priv->tintcolor = tintcolor;
            priv->alpha = alpha;
            gtk_bgbox_set_bg_root(widget, priv);       // sample root pixmap and tint
        } else if (priv->bg_type == BG_INHERIT) {
            gtk_bgbox_set_bg_inherit(widget, priv);    // delegate to parent window's bg
        }
    }

    // Force the widget to repaint with the new background.
    gtk_widget_queue_draw(widget);

    // Notify observers that the "style" property changed.
    // WARNING: this may trigger gtk_bgbox_style_set re-entrantly.
    g_object_notify(G_OBJECT (widget), "style");

    DBG("queue draw all %p\n", widget);
    RET();
}

/*
 * gtk_bgbox_set_bg_root:
 *
 * Private helper; only called from gtk_bgbox_set_background when bg_type == BG_ROOT.
 * Samples the region of the X root-window pixmap that is geometrically beneath
 * this widget, optionally composites a colour tint over it, then installs the
 * result as the window's background pixmap.
 *
 * Parameters:
 *   widget - the GtkBgbox whose background is being set.
 *   priv   - the private data pointer passed in by the caller.
 *            NOTE: despite receiving priv as a parameter, this function
 *            immediately re-fetches it via gtk_bgbox_get_instance_private().
 *            The parameter is therefore redundant and unused, which is a
 *            code-quality issue (minor but confusing).
 *
 * Memory:
 *   priv->pixmap is set to the GdkPixmap returned by fb_bg_get_xroot_pix_for_win().
 *   That function returns a new reference owned by the caller (us).  We store
 *   it in priv->pixmap and release it in gtk_bgbox_set_background (at the top,
 *   on subsequent calls) or in gtk_bgbox_finalize.
 *
 * Fallback:
 *   If the root pixmap cannot be obtained (priv->pixmap is NULL or GDK_NO_BG),
 *   we fall back to the theme style and queue a redraw of the widget's area.
 */
static void
gtk_bgbox_set_bg_root(GtkWidget *widget, GtkBgboxPrivate *priv)
{
    // Re-fetch priv; the parameter 'priv' is shadowed and effectively ignored.
    // This pattern is inconsistent with gtk_bgbox_set_background which passes priv
    // explicitly, but re-fetches it here unnecessarily.
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));

    ENTER;
    // Sample the root pixmap region beneath this widget.
    // Returns a new GdkPixmap ref, or NULL/GDK_NO_BG on failure.
    priv->pixmap = fb_bg_get_xroot_pix_for_win(priv->bg, widget);

    if (!priv->pixmap || priv->pixmap ==  GDK_NO_BG) {
        // Root pixmap unavailable (e.g., compositor not running, or no XROOTPMAP_ID).
        priv->pixmap = NULL;  // normalise GDK_NO_BG sentinel to NULL
        // Fall back to the GTK style background.
        gtk_style_set_background(widget->style, widget->window, widget->state);
        // Invalidate the widget area so GTK will repaint with the style background.
        gtk_widget_queue_draw_area(widget, 0, 0,
              widget->allocation.width, widget->allocation.height);
        DBG("no root pixmap was found\n");
        RET();
    }

    // If a non-zero alpha was requested, composite the tint colour onto the pixmap.
    // alpha == 0 means "no tint"; skip the compositing step for performance.
    if (priv->alpha)
        fb_bg_composite(priv->pixmap, widget->style->black_gc,
              priv->tintcolor, priv->alpha);

    // Install the (possibly tinted) pixmap as the window's X background.
    // FALSE = do not tile relative to parent (use absolute coordinates).
    gdk_window_set_back_pixmap(widget->window, priv->pixmap, FALSE);
    RET();
}

/*
 * gtk_bgbox_set_bg_inherit:
 *
 * Private helper; only called from gtk_bgbox_set_background when
 * bg_type == BG_INHERIT.  Configures the GDK window to inherit its background
 * from the parent window by passing NULL/TRUE to gdk_window_set_back_pixmap.
 *
 * In X11 terms this sets the window's background-pixmap attribute to
 * ParentRelative, meaning the server will use the parent's background when
 * clearing this window -- giving the appearance of transparency.
 *
 * Parameters:
 *   widget - the GtkBgbox.
 *   priv   - passed in by caller but immediately re-fetched (same redundancy
 *             issue as in gtk_bgbox_set_bg_root; the parameter is unused).
 *
 * No pixmap allocation occurs here; the reference held in priv->pixmap should
 * already be NULL at this point (freed at the top of gtk_bgbox_set_background).
 */
static void
gtk_bgbox_set_bg_inherit(GtkWidget *widget, GtkBgboxPrivate *priv)
{
    // Re-fetch priv; incoming parameter is shadowed and unused.
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));

    ENTER;
    // NULL pixmap + TRUE parent-relative = X ParentRelative background.
    // No pixmap to manage; ownership of the parent's background stays with the parent.
    gdk_window_set_back_pixmap(widget->window, NULL, TRUE);
    RET();
}
