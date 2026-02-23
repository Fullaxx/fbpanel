/*
 * main.c - fbpanel System Tray plugin
 *
 * This file implements the fbpanel "tray" plugin, which provides a
 * freedesktop.org-compliant System Notification Area (system tray).
 * It acts as the tray manager host: it instantiates an EggTrayManager
 * that acquires the _NET_SYSTEM_TRAY_S<n> X11 selection and listens for
 * XEMBED dock requests from client applications.  When a client requests
 * to be docked, the manager creates a GtkSocket, embeds the client window
 * into it, and emits "tray_icon_added".  This plugin then packs that socket
 * into a GtkBar (a multi-column/row box widget) so the icons are displayed
 * in the panel.
 *
 * Signals connected to EggTrayManager (must be disconnected when the
 * manager object is destroyed - handled by g_object_unref which triggers
 * finalize -> egg_tray_manager_unmanage):
 *   "tray_icon_added"     -> tray_added()
 *   "tray_icon_removed"   -> tray_removed()
 *   "message_sent"        -> message_sent()
 *   "message_cancelled"   -> message_cancelled()
 *
 * Memory ownership notes:
 *   - tray_priv embeds plugin_instance by value (first member), so it is
 *     allocated/freed by the plugin framework.
 *   - tr->bg is obtained via fb_bg_get_for_display() which returns a
 *     reference-counted singleton; we hold one ref (g_object_unref in
 *     destructor balances it).
 *   - tr->tray_manager is created with egg_tray_manager_new() (refcount=1)
 *     and released with g_object_unref() in the destructor.
 *   - GtkSocket widgets for tray icons are owned by the GtkBar container
 *     after gtk_box_pack_end(); they are destroyed when the container is
 *     destroyed or when plug_removed returns FALSE.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "bg.h"
#include "gtkbgbox.h"
#include "gtkbar.h"

#include "eggtraymanager.h"
#include "fixedtip.h"


//#define DEBUGPRN
#include "dbg.h"


/*
 * tray_priv - private state for the tray plugin instance.
 *
 * The plugin_instance member MUST be first so that a (tray_priv *) can be
 * safely cast to (plugin_instance *) and vice versa - the plugin framework
 * relies on this layout guarantee.
 *
 * Members:
 *   plugin  - base plugin instance (contains panel pointer, pwid, etc.)
 *   box     - GtkBar widget that holds the individual tray icon GtkSockets
 *   tray_manager - EggTrayManager that owns the X11 selection and manages
 *                  XEMBED protocol; NULL if another tray is already running
 *   bg      - FbBg singleton for desktop background change notifications;
 *             we hold one reference, released in destructor
 *   sid     - GObject signal handler ID for the "changed" signal on tr->bg;
 *             must be disconnected before dropping the bg reference
 */
typedef struct {
    plugin_instance plugin;
    GtkWidget *box;
    EggTrayManager *tray_manager;
    FbBg *bg;
    gulong sid;
} tray_priv;

/*
 * tray_bg_changed - callback for desktop background changes.
 *
 * When the desktop background changes we need to repaint any transparent
 * tray icon sockets.  The approach used is a hide/show cycle which forces
 * GTK to re-expose the widget and redraw with the new background.
 *
 * Parameters:
 *   bg     - the FbBg object that emitted "changed" (unused; may be NULL
 *            when called directly from tray_added/tray_removed)
 *   widget - the panel's top-level plugin widget (tr->plugin.pwid)
 *
 * NOTE: gtk_main_iteration() is called conditionally to flush pending
 * expose events between hide and show.  This is a pattern that can cause
 * re-entrant signal delivery and subtle GTK state corruption if any other
 * events arrive during that iteration.  The size-request lock (set_size_request
 * to current size, then back to -1,-1) prevents the widget from resizing
 * during the cycle, but does not prevent re-entrant destruction if the panel
 * is being torn down concurrently.
 */
static void
tray_bg_changed(FbBg *bg, GtkWidget *widget)
{
    ENTER;
    // Lock the widget to its current allocated size to prevent layout shifts
    // during the hide/show redraw cycle.
    gtk_widget_set_size_request(widget, widget->allocation.width,
        widget->allocation.height);
    gtk_widget_hide(widget);
    // Drain one pending event to process expose events before re-showing.
    // BUG: If a destroy event is pending for 'widget', this call may free
    // the widget while we still hold a pointer to it (use-after-free).
    if (gtk_events_pending())
        gtk_main_iteration();
    gtk_widget_show(widget);
    // Restore unconstrained sizing so the widget can respond to future
    // size negotiations normally.
    gtk_widget_set_size_request(widget, -1, -1);
    RET();
}

/*
 * tray_added - signal handler for EggTrayManager "tray_icon_added".
 *
 * Called by EggTrayManager after a tray client has successfully docked its
 * window into a GtkSocket (via XEMBED).  At this point the socket widget is
 * fully realized and the plug is embedded.
 *
 * Parameters:
 *   manager - the EggTrayManager that emitted the signal (unused here)
 *   icon    - the GtkSocket widget whose plug-window is the tray icon;
 *             ownership has already been transferred to the caller by the
 *             signal emission; we take ownership by packing into tr->box
 *   tr      - plugin private data (passed as user_data at connect time)
 *
 * Memory: After gtk_box_pack_end() the socket is owned by tr->box.
 * It will be destroyed when tr->box is destroyed OR when the plug is removed
 * (egg_tray_manager_plug_removed returns FALSE).
 *
 * Side effects: Calls gdk_display_sync() to flush the X queue and ensure the
 * embedding handshake has completed before triggering a background repaint.
 */
static void
tray_added (EggTrayManager *manager, GtkWidget *icon, tray_priv *tr)
{
    ENTER;
    // Pack the new tray icon socket at the end (right/bottom) of the bar.
    // FALSE, FALSE, 0 means: don't expand, don't fill, no padding.
    gtk_box_pack_end(GTK_BOX(tr->box), icon, FALSE, FALSE, 0);
    gtk_widget_show(icon);
    // Synchronize with the X server to ensure XEMBED reparenting is complete
    // before we trigger a repaint; without this the icon may not yet be visible.
    gdk_display_sync(gtk_widget_get_display(icon));
    // Repaint the panel background to account for the new icon's space.
    tray_bg_changed(NULL, tr->plugin.pwid);
    RET();
}

/*
 * tray_removed - signal handler for EggTrayManager "tray_icon_removed".
 *
 * Called when a tray client window has been un-docked (the plug was removed,
 * the client exited, or the socket was explicitly destroyed).  At this point
 * the GtkSocket widget has already been destroyed by the plug_removed handler
 * returning FALSE, so we must NOT touch the socket pointer.
 *
 * Parameters:
 *   manager - the EggTrayManager that emitted the signal (unused)
 *   icon    - the GtkSocket that was removed; may already be destroyed -
 *             DO NOT dereference without a validity check
 *   tr      - plugin private data
 *
 * Side effects: Repaints the panel background to fill the gap left by the
 * removed icon.
 */
static void
tray_removed (EggTrayManager *manager, GtkWidget *icon, tray_priv *tr)
{
    ENTER;
    DBG("del icon\n");
    // Trigger background repaint to redraw the area vacated by the removed icon.
    tray_bg_changed(NULL, tr->plugin.pwid);
    RET();
}

/*
 * message_sent - signal handler for EggTrayManager "message_sent".
 *
 * Called when a tray client has sent a complete balloon-message via the
 * _NET_SYSTEM_TRAY_BEGIN_MESSAGE / _NET_SYSTEM_TRAY_MESSAGE_DATA protocol.
 * This displays a custom tooltip-style popup window positioned near the
 * tray icon using fixed_tip_show().
 *
 * Parameters:
 *   manager - the EggTrayManager that emitted the signal (unused)
 *   icon    - the GtkSocket for the tray icon that sent the message;
 *             we read its X11 window origin to position the tooltip
 *   text    - the complete message string (NUL-terminated, UTF-8);
 *             owned by the EggTrayManager's PendingMessage which is freed
 *             after this signal returns - callee must NOT free or store it
 *   id      - the client-assigned message identifier (for cancellation)
 *   timeout - duration in milliseconds the client requests the message be
 *             shown (currently ignored by fixed_tip_show)
 *   data    - user_data from g_signal_connect (tray_priv *; unused here)
 *
 * FIXME (noted in original code): This does not handle multiple X11 screens
 * (multihead) - it always passes screen 0 to fixed_tip_show.
 *
 * NOTE: icon->window accesses the GdkWindow directly via the deprecated
 * GTK2 widget->window field; this is not compatible with GTK3.
 */
static void
message_sent (EggTrayManager *manager, GtkWidget *icon, const char *text,
    glong id, glong timeout, void *data)
{
    /* FIXME multihead */
    int x, y;

    ENTER;
    // Get the absolute screen coordinates of the tray icon window so we
    // can position the balloon message popup adjacent to the icon.
    gdk_window_get_origin (icon->window, &x, &y);
    // Display the balloon tooltip; strut=screen_height-50 positions it
    // relative to a hypothetical bottom panel edge.  Hardcoding screen_height-50
    // is incorrect for top panels or non-standard panel positions.
    fixed_tip_show (0, x, y, FALSE, gdk_screen_height () - 50, text);
    RET();
}

/*
 * message_cancelled - signal handler for EggTrayManager "message_cancelled".
 *
 * Called when a tray client cancels a previously sent balloon message via
 * _NET_SYSTEM_TRAY_CANCEL_MESSAGE.  In a complete implementation this would
 * hide the popup if it corresponds to the cancelled message id.
 *
 * Parameters:
 *   manager - the EggTrayManager that emitted the signal (unused)
 *   icon    - the GtkSocket for the icon that sent the cancellation (unused)
 *   id      - the message identifier to cancel (unused - not implemented)
 *   data    - user_data from g_signal_connect (unused)
 *
 * NOTE: This is a stub - it does not actually hide the balloon or check
 * the message id.  If the tooltip is visible for a different message it
 * will not be cancelled even when requested.
 */
static void
message_cancelled (EggTrayManager *manager, GtkWidget *icon, glong id,
    void *data)
{
    ENTER;
    // TODO: call fixed_tip_hide() if the displayed message matches 'id'.
    RET();
}

/*
 * tray_destructor - clean up and release all resources for the tray plugin.
 *
 * Called by the fbpanel plugin framework when the plugin is being unloaded.
 * Must undo everything done in tray_constructor in reverse order.
 *
 * Parameters:
 *   p - pointer to the plugin_instance; cast to tray_priv * because
 *       plugin_instance is the first member of tray_priv (guaranteed layout)
 *
 * Signal disconnection:
 *   tr->sid (connected to tr->bg "changed") is explicitly disconnected here.
 *   The four signals connected to tr->tray_manager are implicitly disconnected
 *   when g_object_unref drops the refcount to zero and triggers finalize(),
 *   which calls egg_tray_manager_unmanage().  This is safe because
 *   GObject signal connections from external objects to a GObject are
 *   automatically invalidated when the emitting object is finalized.
 *
 * Memory release order:
 *   1. Disconnect bg signal (must precede unref to avoid stale callback)
 *   2. Unref bg singleton (balance the ref taken in constructor)
 *   3. Unref tray_manager (triggers finalize -> unmanage -> X selection release)
 *   4. Hide tooltip window
 *
 * NOTE: The four GObject signals connected to tr->tray_manager in
 * tray_constructor are NOT explicitly disconnected here.  This is safe
 * only because tray_priv (the signal target's user_data) lives until after
 * g_object_unref(tr->tray_manager) completes finalization.  However, if any
 * signal were emitted *during* finalization (e.g. LOST_SELECTION), the
 * callback would receive a partially-destroyed tray_priv.
 */
static void
tray_destructor(plugin_instance *p)
{
    tray_priv *tr = (tray_priv *) p;

    ENTER;
    // Disconnect the background "changed" signal before releasing tr->bg to
    // prevent a stale callback firing during or after unref.
    g_signal_handler_disconnect(tr->bg, tr->sid);
    // Release our reference to the FbBg singleton.
    g_object_unref(tr->bg);
    /* Make sure we drop the manager selection */
    // Dropping the last reference triggers egg_tray_manager_finalize() which
    // calls egg_tray_manager_unmanage(), releasing the X11 selection and
    // removing the GDK window filter.  Tray client sockets are destroyed when
    // their parent GtkBar (tr->box) is destroyed by the plugin framework.
    if (tr->tray_manager)
        g_object_unref(G_OBJECT(tr->tray_manager));
    // Destroy the balloon message popup window if currently visible.
    fixed_tip_hide();
    RET();
}


/*
 * tray_size_alloc - GtkWidget "size-allocate" handler for the alignment widget.
 *
 * Called each time the alignment container receives a new size allocation.
 * Computes how many icon slots fit in the allocated dimension perpendicular
 * to the panel's orientation, then updates the GtkBar's column/row count.
 *
 * Parameters:
 *   widget - the GtkAlignment widget that was allocated (unused by name, but
 *            the allocation 'a' describes its new geometry)
 *   a      - the new GtkAllocation for the alignment widget
 *   tr     - plugin private data (passed as user_data)
 *
 * Logic:
 *   For a horizontal panel the bar fills horizontally; icons are sized by
 *   max_elem_height.  The number of rows the bar can display is
 *   floor(allocated_height / icon_size).  For a vertical panel the number
 *   of columns is floor(allocated_width / icon_size).
 *
 * NOTE: Division by zero if max_elem_height is 0; no guard present.
 */
static void
tray_size_alloc(GtkWidget *widget, GtkAllocation *a,
    tray_priv *tr)
{
    int dim, size;

    ENTER;
    // Retrieve the configured maximum element height from the panel, which
    // is used as the square icon cell size.
    size = tr->plugin.panel->max_elem_height;
    if (tr->plugin.panel->orientation == GTK_ORIENTATION_HORIZONTAL)
        // Horizontal panel: icons lay out left-to-right, so the number of
        // "rows" (vertical slots) is constrained by the bar's height.
        dim = a->height / size;
    else
        // Vertical panel: icons lay out top-to-bottom, so the number of
        // "columns" is constrained by the bar's width.
        dim = a->width / size;
    DBG("width=%d height=%d iconsize=%d -> dim=%d\n",
        a->width, a->height, size, dim);
    // Tell the GtkBar how many rows or columns to use for icon layout.
    gtk_bar_set_dimension(GTK_BAR(tr->box), dim);
    RET();
}


/*
 * tray_constructor - initialize and register the tray plugin.
 *
 * Called by the fbpanel plugin framework when the plugin is loaded.  Builds
 * the widget hierarchy, connects to the FbBg background change notification,
 * and starts the EggTrayManager to claim the system tray X11 selection.
 *
 * Parameters:
 *   p - plugin_instance allocated by the framework with priv_size bytes
 *       (sizeof(tray_priv)); the first member is plugin_instance so the
 *       cast to tray_priv* is valid.
 *
 * Returns:
 *   1 on success (even if another tray is running - a partial init path),
 *   but does NOT return 0 on failure; the caller treats non-zero as success.
 *   NOTE: returning 1 when egg_tray_manager_check_running() is true means the
 *   plugin "succeeds" while tr->tray_manager is NULL - the tray simply shows
 *   no icons.  This is intentional degraded-mode behaviour.
 *
 * Widget hierarchy created:
 *   p->pwid (provided by framework)
 *     └─ GtkAlignment (ali) - centers the bar; "size-allocate" -> tray_size_alloc
 *          └─ GtkBar (tr->box) - multi-row/column icon container
 *               └─ GtkSocket widgets (added dynamically by tray_added)
 *
 * Signals connected (and where they are disconnected):
 *   ali    "size-allocate" -> tray_size_alloc  [disconnected when ali destroyed]
 *   tr->bg "changed"       -> tray_bg_changed  [disconnected in tray_destructor]
 *   tr->tray_manager "tray_icon_added"     -> tray_added    [auto on unref]
 *   tr->tray_manager "tray_icon_removed"   -> tray_removed  [auto on unref]
 *   tr->tray_manager "message_sent"        -> message_sent  [auto on unref]
 *   tr->tray_manager "message_cancelled"   -> message_cancelled [auto on unref]
 *
 * Memory: tr->bg ref is obtained here; tr->tray_manager is created here.
 * Both are released in tray_destructor.
 */
static int
tray_constructor(plugin_instance *p)
{
    tray_priv *tr;
    GdkScreen *screen;
    GtkWidget *ali;

    ENTER;
    tr = (tray_priv *) p;
    // Register plugin class metadata with the panel framework.
    class_get("tray");
    // Create a centering alignment widget with no padding and no expansion.
    // xalign=0.5, yalign=0.5, xscale=0, yscale=0 means the child is
    // centered and not stretched.
    ali = gtk_alignment_new(0.5, 0.5, 0, 0);
    // Track size changes so we can recalculate the GtkBar grid dimensions.
    g_signal_connect(G_OBJECT(ali), "size-allocate",
        (GCallback) tray_size_alloc, tr);
    gtk_container_set_border_width(GTK_CONTAINER(ali), 0);
    gtk_container_add(GTK_CONTAINER(p->pwid), ali);
    // Create the icon bar with the panel's orientation and icon size.
    // Both min_size and max_size are set to max_elem_height to enforce
    // square cells for tray icons.
    tr->box = gtk_bar_new(p->panel->orientation, 0,
        p->panel->max_elem_height, p->panel->max_elem_height);
    gtk_container_add(GTK_CONTAINER(ali), tr->box);
    gtk_container_set_border_width(GTK_CONTAINER (tr->box), 0);
    gtk_widget_show_all(ali);
    // Obtain (or create) the per-display FbBg singleton and hold a reference.
    // The "changed" signal fires when the desktop wallpaper changes, so we
    // can repaint transparent tray icon backgrounds.
    tr->bg = fb_bg_get_for_display();
    tr->sid = g_signal_connect(tr->bg, "changed",
        G_CALLBACK(tray_bg_changed), p->pwid);

    // Determine which X11 screen the panel is displayed on.
    screen = gtk_widget_get_screen(p->panel->topgwin);

    // If another system tray is already managing this screen's selection,
    // bail out gracefully rather than fighting for the selection.
    if (egg_tray_manager_check_running(screen)) {
        tr->tray_manager = NULL;
        g_message("tray: another systray already running — tray plugin inactive");
        // Return 1 (success) so the plugin remains loaded but inactive.
        // tr->tray_manager is NULL; tray_destructor handles this safely.
        RET(1);
    }
    // Create the tray manager object (refcount = 1).
    tr->tray_manager = egg_tray_manager_new ();
    // Attempt to acquire the _NET_SYSTEM_TRAY_S<n> X11 selection and start
    // listening for XEMBED dock requests on this screen.
    if (!egg_tray_manager_manage_screen (tr->tray_manager, screen))
        g_printerr("tray: can't get the system tray manager selection\n");

    // Connect all EggTrayManager signals.  The handler callbacks reference
    // 'tr' (tray_priv), which outlives the tray_manager (we unref the manager
    // in the destructor before the plugin instance is freed).
    g_signal_connect(tr->tray_manager, "tray_icon_added",
        G_CALLBACK(tray_added), tr);
    g_signal_connect(tr->tray_manager, "tray_icon_removed",
        G_CALLBACK(tray_removed), tr);
    g_signal_connect(tr->tray_manager, "message_sent",
        G_CALLBACK(message_sent), tr);
    g_signal_connect(tr->tray_manager, "message_cancelled",
        G_CALLBACK(message_cancelled), tr);

    gtk_widget_show_all(tr->box);
    RET(1);

}


/*
 * class - static plugin_class descriptor for the "tray" plugin.
 *
 * This structure is read by the fbpanel plugin loader to discover the
 * plugin's type name, display name, version, description, instance size,
 * and constructor/destructor function pointers.
 *
 * priv_size = sizeof(tray_priv): the framework allocates this many bytes
 * for each plugin instance, with the first sizeof(plugin_instance) bytes
 * used as the base plugin_instance fields.
 */
static plugin_class class = {
    .count       = 0,
    .type        = "tray",
    .name        = "System tray",
    .version     = "1.0",
    .description = "System tray aka Notification Area",
    .priv_size   = sizeof(tray_priv),

    .constructor = tray_constructor,
    .destructor = tray_destructor,
};
// class_ptr is the exported symbol that the plugin loader looks for when
// loading this .so; it points to the static class descriptor above.
static plugin_class *class_ptr = (plugin_class *) &class;
