/* eggtraymanager.c
 * Copyright (C) 2002 Anders Carlsson <andersca@gnu.org>
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
 * eggtraymanager.c - System Tray Protocol host-side implementation
 *
 * This file implements the freedesktop.org System Tray Protocol manager.
 * Key responsibilities:
 *   - Acquire _NET_SYSTEM_TRAY_S<n> X11 selection ownership
 *   - Receive _NET_SYSTEM_TRAY_OPCODE ClientMessage events (dock requests,
 *     balloon messages, message cancellations)
 *   - Create GtkSocket widgets and embed tray client windows via XEMBED
 *   - Reassemble multi-packet balloon messages from MESSAGE_DATA events
 *   - Emit GObject signals to notify the host application (fbpanel main.c)
 *   - Release the selection and clean up on finalize or SelectionClear
 *
 * XEMBED protocol (XEmbed spec, freedesktop.org):
 *   When a client requests docking, we call gtk_socket_add_id(socket, xid).
 *   GtkSocket then sends XEMBED messages (via ClientMessage) to the plug
 *   window to negotiate embedding: XEMBED_EMBEDDED_NOTIFY, XEMBED_FOCUS_IN,
 *   XEMBED_FOCUS_OUT, XEMBED_WINDOW_ACTIVATE, etc.  The plug reparents its
 *   window into the socket's X window.  GTK handles most of this internally.
 *
 * GTK2 API usage (not compatible with GTK3):
 *   - GTK_WIDGET_NO_WINDOW() macro (use gtk_widget_get_has_window() in GTK3)
 *   - widget->window direct field access (use gtk_widget_get_window() in GTK3)
 *   - gdk_window_set_back_pixmap() (removed in GTK3; use cairo surfaces)
 *   - GDK_DISPLAY() macro (use gdk_display_get_default() in GTK3)
 *   - GDK_WINDOW_XWINDOW() macro (use GDK_WINDOW_XID() in GTK3)
 *   - GTK_WIDGET_REALIZED() macro (use gtk_widget_get_realized() in GTK3)
 *   - gtk_widget_size_request() (use gtk_widget_get_preferred_size() in GTK3)
 *   - gdk_x11_lookup_xdisplay() (still available in GTK3 via GDK X11)
 */

#include <string.h>
#include <gdk/gdkx.h>
#include <gtk/gtkinvisible.h>
#include <gtk/gtksocket.h>
#include <gtk/gtkwindow.h>
#include "eggtraymanager.h"
#include "eggmarshalers.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * Signal index enum - array indices into manager_signals[].
 * LAST_SIGNAL is used to size the array; keep in sync with
 * egg_tray_manager_class_init signal registrations.
 */
enum
{
  TRAY_ICON_ADDED,
  TRAY_ICON_REMOVED,
  MESSAGE_SENT,
  MESSAGE_CANCELLED,
  LOST_SELECTION,
  LAST_SIGNAL
};

/*
 * PendingMessage - partial balloon message awaiting full data delivery.
 *
 * The _NET_SYSTEM_TRAY_BEGIN_MESSAGE opcode starts a balloon message.
 * Since each X ClientMessage can carry only 20 bytes of data, large messages
 * are split across multiple _NET_SYSTEM_TRAY_MESSAGE_DATA events.  A
 * PendingMessage accumulates the fragments until remaining_len == 0.
 *
 * Fields:
 *   id            - client-assigned message identifier (for cancellation)
 *   len           - total expected message length in bytes
 *   remaining_len - bytes still to be received (counts down from len)
 *   timeout       - client-requested display duration in milliseconds;
 *                   0 means "display until dismissed"
 *   window        - X Window ID of the sending tray client (used to match
 *                   subsequent MESSAGE_DATA events to this pending entry)
 *   str           - heap-allocated buffer of size (len+1); partially filled
 *                   as MESSAGE_DATA events arrive; NUL-terminated up front
 *                   (str[len] = '\0' set at allocation time)
 *
 * Memory: allocated via g_new0, freed by pending_message_free().
 * str is allocated via g_malloc and freed in pending_message_free().
 */
typedef struct
{
  long id, len;
  long remaining_len;

  long timeout;
  Window window;
  char *str;
} PendingMessage;

/* Cached parent class pointer, set in egg_tray_manager_class_init. */
static GObjectClass *parent_class = NULL;

/*
 * manager_signals - array of signal IDs registered in class_init.
 * Indexed by the enum above.  Used with g_signal_emit() to fire signals.
 */
static guint manager_signals[LAST_SIGNAL] = { 0 };

/*
 * _NET_SYSTEM_TRAY_OPCODE values (data.l[1] of the ClientMessage).
 * These are defined by the freedesktop.org System Tray Protocol spec.
 */
#define SYSTEM_TRAY_REQUEST_DOCK    0  /* client wants to embed its window */
#define SYSTEM_TRAY_BEGIN_MESSAGE   1  /* client starts a balloon message */
#define SYSTEM_TRAY_CANCEL_MESSAGE  2  /* client cancels a balloon message */

/* Forward declarations for functions used before they are defined. */
static gboolean egg_tray_manager_check_running_xscreen (Screen *xscreen);

static void egg_tray_manager_init (EggTrayManager *manager);
static void egg_tray_manager_class_init (EggTrayManagerClass *klass);

static void egg_tray_manager_finalize (GObject *object);

static void egg_tray_manager_unmanage (EggTrayManager *manager);

/*
 * egg_tray_manager_get_type - register and return the GType for EggTrayManager.
 *
 * Uses a local static to ensure type registration happens only once.
 * Not thread-safe (no mutex around the our_type == 0 check), but safe in
 * single-threaded GTK2 usage.
 *
 * Returns: the GType for EggTrayManager (always the same value after first call).
 */
GType
egg_tray_manager_get_type (void)
{
  static GType our_type = 0;

  if (our_type == 0)
    {
      static const GTypeInfo our_info =
      {
	sizeof (EggTrayManagerClass),
	(GBaseInitFunc) NULL,
	(GBaseFinalizeFunc) NULL,
	(GClassInitFunc) egg_tray_manager_class_init,
	NULL, /* class_finalize */
	NULL, /* class_data */
	sizeof (EggTrayManager),
	0,    /* n_preallocs */
	(GInstanceInitFunc) egg_tray_manager_init
      };

      // Register EggTrayManager as a GObject subtype; 0 flags = non-abstract.
      our_type = g_type_register_static (G_TYPE_OBJECT, "EggTrayManager", &our_info, 0);
    }

  return our_type;

}

/*
 * egg_tray_manager_init - GObject instance initializer.
 *
 * Called by the GObject type system for each new EggTrayManager instance,
 * after the instance memory has been zeroed.
 *
 * Parameters:
 *   manager - the newly allocated instance (all fields zero-initialized)
 *
 * Initializes:
 *   socket_table - a direct-hash, direct-equality hash table mapping
 *                  (GINT_TO_POINTER(Window) -> GtkSocket *).
 *                  Key destroy notify: none (integer key, no heap allocation).
 *                  Value destroy notify: none (socket owned by its container).
 */
static void
egg_tray_manager_init (EggTrayManager *manager)
{
  // NULL key/value hash/compare functions use g_direct_hash / g_direct_equal,
  // appropriate for integer (XID) keys stored via GINT_TO_POINTER.
  manager->socket_table = g_hash_table_new (NULL, NULL);
}

/*
 * egg_tray_manager_class_init - GObject class initializer for EggTrayManager.
 *
 * Sets up the GObject vtable override (finalize) and registers all five
 * signals using the custom marshalers from eggmarshalers.h.
 *
 * Parameters:
 *   klass - the EggTrayManagerClass being initialized
 *
 * Signals registered here (indices match the enum above):
 *
 *   TRAY_ICON_ADDED   "tray_icon_added"   (manager, GtkSocket*)
 *   TRAY_ICON_REMOVED "tray_icon_removed" (manager, GtkSocket*)
 *   MESSAGE_SENT      "message_sent"      (manager, GtkSocket*, string, long, long)
 *   MESSAGE_CANCELLED "message_cancelled" (manager, GtkSocket*, long)
 *   LOST_SELECTION    "lost_selection"    (manager)
 *
 * All signals use G_SIGNAL_RUN_LAST, meaning class handler runs after all
 * connected handlers; suitable for signals that do "notification" work.
 */
static void
egg_tray_manager_class_init (EggTrayManagerClass *klass)
{
    GObjectClass *gobject_class;

    // Cache the parent (GObject) class pointer for use in finalize chain call.
    parent_class = g_type_class_peek_parent (klass);
    gobject_class = (GObjectClass *)klass;

    // Override finalize to clean up X11 resources before GObject memory release.
    gobject_class->finalize = egg_tray_manager_finalize;

    // Register "tray_icon_added": emitted when a tray client is successfully embedded.
    // Signature: void handler(EggTrayManager*, GtkSocket*, gpointer user_data)
    manager_signals[TRAY_ICON_ADDED] =
        g_signal_new ("tray_icon_added",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, tray_icon_added),
              NULL, NULL,
              g_cclosure_marshal_VOID__OBJECT,
              G_TYPE_NONE, 1,
              GTK_TYPE_SOCKET);

    // Register "tray_icon_removed": emitted when a tray client's window is removed.
    // Signature: void handler(EggTrayManager*, GtkSocket*, gpointer user_data)
    manager_signals[TRAY_ICON_REMOVED] =
        g_signal_new ("tray_icon_removed",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, tray_icon_removed),
              NULL, NULL,
              g_cclosure_marshal_VOID__OBJECT,
              G_TYPE_NONE, 1,
              GTK_TYPE_SOCKET);

    // Register "message_sent": emitted when a complete balloon message is assembled.
    // Signature: void handler(EggTrayManager*, GtkSocket*, gchar*, glong id, glong timeout, gpointer)
    // Uses custom marshaler for the OBJECT+STRING+LONG+LONG parameter combination.
    manager_signals[MESSAGE_SENT] =
        g_signal_new ("message_sent",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, message_sent),
              NULL, NULL,
              _egg_marshal_VOID__OBJECT_STRING_LONG_LONG,
              G_TYPE_NONE, 4,
              GTK_TYPE_SOCKET,
              G_TYPE_STRING,
              G_TYPE_LONG,
              G_TYPE_LONG);

    // Register "message_cancelled": emitted when a client cancels a balloon message.
    // Signature: void handler(EggTrayManager*, GtkSocket*, glong id, gpointer)
    // Uses custom marshaler for OBJECT+LONG parameter combination.
    manager_signals[MESSAGE_CANCELLED] =
        g_signal_new ("message_cancelled",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, message_cancelled),
              NULL, NULL,
              _egg_marshal_VOID__OBJECT_LONG,
              G_TYPE_NONE, 2,
              GTK_TYPE_SOCKET,
              G_TYPE_LONG);

    // Register "lost_selection": emitted when another process steals our X11 selection.
    // Signature: void handler(EggTrayManager*, gpointer user_data)
    manager_signals[LOST_SELECTION] =
        g_signal_new ("lost_selection",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, lost_selection),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);

}

/*
 * egg_tray_manager_finalize - GObject finalize handler.
 *
 * Called by the GObject reference counting system when the last reference
 * to the EggTrayManager is dropped.  Releases all X11 and GTK resources
 * before delegating to the parent finalize.
 *
 * Parameters:
 *   object - the GObject being finalized; cast to EggTrayManager *
 *
 * Note: After egg_tray_manager_unmanage() the invisible window is destroyed
 * and manager->invisible is set to NULL.  Pending messages are NOT freed
 * here - this is a bug; memory is leaked if messages are in flight when
 * the manager is finalized.
 */
static void
egg_tray_manager_finalize (GObject *object)
{
  EggTrayManager *manager;

  manager = EGG_TRAY_MANAGER (object);

  // Release X11 selection, destroy the invisible window, remove GDK filter.
  egg_tray_manager_unmanage (manager);

  // Chain up to GObject's finalize to free the GObject memory itself.
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/*
 * egg_tray_manager_new - construct a new EggTrayManager.
 *
 * Allocates and zero-initializes a new EggTrayManager instance via the
 * GObject type system (triggers egg_tray_manager_init for instance init).
 *
 * Returns: new EggTrayManager * with refcount=1.  Caller must g_object_unref().
 */
EggTrayManager *
egg_tray_manager_new (void)
{
  EggTrayManager *manager;

  manager = g_object_new (EGG_TYPE_TRAY_MANAGER, NULL);

  return manager;
}

/*
 * egg_tray_manager_plug_removed - GtkSocket "plug_removed" signal handler.
 *
 * Called by GTK's socket/plug mechanism when the embedded plug window
 * disconnects (client exited, window destroyed, or XEMBED unembedding).
 * This is the primary cleanup path for tray icon removal.
 *
 * Parameters:
 *   socket  - the GtkSocket whose plug was removed
 *   manager - the EggTrayManager (passed as user_data)
 *
 * Returns: FALSE, which tells GtkSocket to destroy itself.  Returning TRUE
 *          would keep the socket widget alive (an empty socket placeholder).
 *
 * Actions:
 *   1. Removes the socket from socket_table (keyed by the embedded XID).
 *   2. Clears the "egg-tray-child-window" data from the socket to prevent
 *      use-after-free if the signal is emitted while destructors run.
 *   3. Emits "tray_icon_removed" to notify the host application.
 *   4. Returns FALSE to cause GtkSocket self-destruction.
 *
 * Memory: The Window * stored under "egg-tray-child-window" is freed by
 * g_object_set_data with a NULL value, which triggers the g_free destroy
 * notify registered in g_object_set_data_full.
 *
 * XEMBED note: By the time this callback fires, the plug window has already
 * been de-embedded by X.  GtkSocket has sent XEMBED_EMBEDDED_NOTIFY with
 * a None embedder, and the socket's plug_window field is cleared.
 */
static gboolean
egg_tray_manager_plug_removed (GtkSocket       *socket,
    EggTrayManager  *manager)
{
    Window *window;

    ENTER;
    // Retrieve the X Window ID that was embedded in this socket.
    // This was stored as heap-allocated (Window *) so it survives widget destruction.
    window = g_object_get_data (G_OBJECT (socket), "egg-tray-child-window");

    // Remove this socket from the XID->socket mapping table.
    // GINT_TO_POINTER is safe here because Window (XID) is an unsigned long;
    // on 64-bit systems only the lower 32 bits are used, which is sufficient
    // for X11 XIDs (they are 29-bit values per the X protocol).
    g_hash_table_remove (manager->socket_table, GINT_TO_POINTER (*window));

    // Clear the object data; this triggers g_free on the Window * buffer via
    // the destroy notify registered in g_object_set_data_full.
    g_object_set_data (G_OBJECT (socket), "egg-tray-child-window",
        NULL);

    // Notify the host application that this tray icon has been removed.
    g_signal_emit (manager, manager_signals[TRAY_ICON_REMOVED], 0, socket);

    /* This destroys the socket. */
    // Returning FALSE causes GtkSocket to destroy itself (gtk_widget_destroy).
    RET(FALSE);
}

/*
 * egg_tray_manager_socket_exposed - GtkWidget "expose_event" handler for sockets.
 *
 * Handles expose (redraw) events for the GtkSocket widget that hosts a tray
 * icon.  Clears the exposed area to the parent window's background, enabling
 * visual transparency for tray icons that expect to see the desktop/panel
 * wallpaper behind them.
 *
 * Parameters:
 *   widget    - the GtkSocket widget receiving the expose event
 *   event     - the GdkEventExpose describing the damaged area
 *   user_data - unused
 *
 * Returns: FALSE to allow normal GTK expose processing to continue after
 *          the clear (other signal handlers and child draws will still run).
 *
 * NOTE: gdk_window_clear_area() uses the window's background to clear,
 * which was set to NULL+TRUE (inherit from parent) in
 * egg_tray_manager_make_socket_transparent.  This achieves background
 * inheritance through the X window hierarchy.
 *
 * GTK2/GTK3 note: GdkEventExpose and gdk_window_clear_area() do not exist
 * in GTK3; this must be replaced with a cairo-based draw callback.
 */
static gboolean
egg_tray_manager_socket_exposed (GtkWidget      *widget,
      GdkEventExpose *event,
      gpointer        user_data)
{
    ENTER;
    // Clear only the damaged region (event->area) rather than the whole socket,
    // using the window's inherited background pixmap (set to parent's background).
    gdk_window_clear_area (widget->window,
          event->area.x, event->area.y,
          event->area.width, event->area.height);
    RET(FALSE);
}



/*
 * egg_tray_manager_make_socket_transparent - make a widget's X window
 * inherit its parent window's background.
 *
 * Called on the GtkSocket at "realize" time and on all child widgets.
 * Sets the X window background pixmap to None with inherit=TRUE, which
 * causes the X server to copy the parent's background into the window
 * automatically on expose events (ParentRelative background).
 *
 * Parameters:
 *   widget    - the widget whose X window should become transparent
 *   user_data - unused
 *
 * Returns: void (used as a gtk_container_foreach callback)
 *
 * Skips NO_WINDOW widgets (e.g. GtkLabel) since they share their parent's
 * GdkWindow and do not have their own X window to configure.
 *
 * GTK2 API: GTK_WIDGET_NO_WINDOW(), gdk_window_set_back_pixmap(),
 * widget->window.  All removed/changed in GTK3.
 */
static void
egg_tray_manager_make_socket_transparent (GtkWidget *widget,
      gpointer   user_data)
{
    ENTER;
    // Skip widgets that do not have their own GdkWindow (share parent's window).
    if (GTK_WIDGET_NO_WINDOW (widget))
        RET();
    // NULL pixmap + TRUE (parent_relative) = use parent window's background.
    // This is the classic GTK2 pattern for pseudo-transparency.
    gdk_window_set_back_pixmap (widget->window, NULL, TRUE);
    RET();
}



/*
 * egg_tray_manager_socket_style_set - GtkWidget "style_set" handler for sockets.
 *
 * When the GTK style changes (e.g. theme switch), the background pixmap
 * may be reset by GTK.  This handler re-applies transparency after each
 * style change.
 *
 * Parameters:
 *   widget         - the GtkSocket whose style changed
 *   previous_style - the old GtkStyle (unused)
 *   user_data      - unused
 *
 * Skips the call if widget->window is NULL, which can happen if the style
 * changes before the widget is realized.
 */
static void
egg_tray_manager_socket_style_set (GtkWidget *widget,
      GtkStyle  *previous_style,
      gpointer   user_data)
{
    ENTER;
    // Guard: widget may not be realized yet; nothing to do if no window.
    if (widget->window == NULL)
        RET();
    // Re-apply transparent background after the style change.
    egg_tray_manager_make_socket_transparent(widget, user_data);
    RET();
}

/*
 * egg_tray_manager_handle_dock_request - process SYSTEM_TRAY_REQUEST_DOCK.
 *
 * Called when a tray client sends a _NET_SYSTEM_TRAY_OPCODE ClientMessage
 * with opcode 0 (SYSTEM_TRAY_REQUEST_DOCK).  The client's X window ID is
 * in xevent->data.l[2].
 *
 * XEMBED embedding sequence:
 *   1. Create a GtkSocket widget and show it (not yet added to any container).
 *   2. Connect transparency and plug-removed signal handlers.
 *   3. Emit "tray_icon_added" so the host can pack the socket into its layout.
 *      If the host does NOT add the socket to a container with a realized
 *      toplevel, we abort (the socket was never embedded anywhere useful).
 *   4. Call gtk_socket_add_id() to initiate XEMBED: GTK reparents the client
 *      window into the socket's X window and exchanges XEMBED protocol messages.
 *   5. Verify the client window still exists via XGetWindowAttributes.
 *   6. Insert the socket into socket_table for later lookup.
 *   7. Call gtk_widget_size_request to ensure the socket requests space.
 *
 * Error path (socket has no toplevel window, or XGetWindowAttributes fails):
 *   Emit "tray_icon_removed" and destroy the socket.
 *
 * Parameters:
 *   manager - the EggTrayManager handling the request
 *   xevent  - the XClientMessageEvent; data.l[2] is the client's X Window ID
 *
 * Memory:
 *   'window' is heap-allocated (g_new(Window, 1)) and attached to the socket
 *   with g_object_set_data_full(..., g_free) as destroy notify.  The socket
 *   itself is owned by whatever container packs it (host application).
 *
 * Race condition / XEMBED note:
 *   Between receiving REQUEST_DOCK and calling gtk_socket_add_id(), the
 *   client window could be destroyed.  gdk_error_trap_push/pop around
 *   XGetWindowAttributes detects this, but the plug_removed signal should
 *   also handle late destruction gracefully.
 *
 * BUG: g_signal_emit(TRAY_ICON_ADDED) is called BEFORE gtk_socket_add_id().
 * This means the signal handler (tray_added in main.c) packs the socket
 * into a container before XEMBED embedding has started.  The check
 * GTK_IS_WINDOW(gtk_widget_get_toplevel(socket)) then validates that packing
 * succeeded.  However, calling gdk_display_sync() in tray_added before
 * gtk_socket_add_id() is called means the sync has no XEMBED handshake to
 * flush yet.
 *
 * BUG: The "error:" label is jumped to via `goto error` from inside the
 * if-block, but the label is OUTSIDE the if-block, meaning the code below
 * the label runs in BOTH the success path (via RET() which returns early)
 * AND the error path.  The RET() inside the if-block prevents fall-through
 * on success, so the logic is correct but confusing and fragile.
 *
 * BUG: On the error path, g_signal_emit(TRAY_ICON_REMOVED) is called even
 * though the socket was never successfully added (TRAY_ICON_ADDED was emitted,
 * but if the host didn't pack it, it's possible the removed signal surprises
 * the host).  The socket is also gtk_widget_destroy()'d which fires plug_removed
 * if the plug was already attached - potential double-removal signal.
 */
static void
egg_tray_manager_handle_dock_request(EggTrayManager *manager,
    XClientMessageEvent  *xevent)
{
    GtkWidget *socket;
    Window *window;

    ENTER;
    // Create a new GtkSocket to host the tray client's X window.
    socket = gtk_socket_new ();
    // app_paintable=TRUE prevents GTK from erasing the socket background
    // before expose events, which would interfere with our transparency trick.
    gtk_widget_set_app_paintable (socket, TRUE);
    // Disable double-buffering so that expose events are forwarded directly
    // to the window without an intermediate pixmap; necessary for transparency.
    gtk_widget_set_double_buffered (socket, FALSE);
    // Request expose events so our expose handler can clear the background.
    gtk_widget_add_events (socket, GDK_EXPOSURE_MASK);

    // On realize: set the socket window's background to inherit from parent.
    g_signal_connect (socket, "realize",
          G_CALLBACK (egg_tray_manager_make_socket_transparent), NULL);
    // On expose: clear the exposed area with the parent background.
    g_signal_connect (socket, "expose_event",
          G_CALLBACK (egg_tray_manager_socket_exposed), NULL);
    // After style changes: re-apply background transparency.
    // connect_after so the style change is fully applied before we override.
    g_signal_connect_after (socket, "style_set",
          G_CALLBACK (egg_tray_manager_socket_style_set), NULL);
    gtk_widget_show (socket);


    /* We need to set the child window here
     * so that the client can call _get functions
     * in the signal handler
     */
    // Allocate heap storage for the Window XID so it has a stable address
    // that can be stored as GObject data and outlives the xevent stack frame.
    window = g_new (Window, 1);
    *window = xevent->data.l[2];
    DBG("plug window %lx\n", *window);
    // Attach the client's XID to the socket object; g_free is the destroy
    // notify, so it will be freed when the socket is finalized or when
    // g_object_set_data is called with NULL (as done in plug_removed).
    g_object_set_data_full (G_OBJECT (socket), "egg-tray-child-window",
        window, g_free);

    // Emit "tray_icon_added" BEFORE gtk_socket_add_id().  This allows the
    // host to pack the socket into a container so it gets a realized toplevel.
    // The socket must have a realized parent window before XEMBED can work.
    g_signal_emit(manager, manager_signals[TRAY_ICON_ADDED], 0,
        socket);

    /* Add the socket only if it's been attached */
    // After the signal, check if the socket was packed into a window hierarchy.
    // gtk_widget_get_toplevel returns the topmost ancestor; GTK_IS_WINDOW
    // checks it's an actual window (not just the socket itself, which would
    // be returned if the socket has no parent).
    if (GTK_IS_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(socket)))) {
        GtkRequisition req;
        XWindowAttributes wa;

        DBG("socket has window. going on\n");
        // Initiate XEMBED: reparent the client's X window into this socket.
        // After this call GTK exchanges XEMBED_EMBEDDED_NOTIFY with the plug.
        gtk_socket_add_id(GTK_SOCKET (socket), xevent->data.l[2]);

        // Connect the plug_removed callback AFTER gtk_socket_add_id() so we
        // only track removal once the plug is actually attached.
        g_signal_connect(socket, "plug_removed",
              G_CALLBACK(egg_tray_manager_plug_removed), manager);

        // Verify the client window still exists (race: it may have been
        // destroyed between the dock request and now).
        gdk_error_trap_push();
        XGetWindowAttributes(GDK_DISPLAY(), *window, &wa);
        if (gdk_error_trap_pop()) {
            // X error occurred (e.g. BadWindow): the client window is gone.
            ERR("can't embed window %lx\n", xevent->data.l[2]);
            goto error;
        }
        // Register the socket in the XID->socket lookup table for
        // balloon message routing and message cancellation.
        g_hash_table_insert(manager->socket_table,
            GINT_TO_POINTER(xevent->data.l[2]), socket);
        // Prime the size request so the socket participates in layout.
        // req.width = req.height = 1 is a minimal hint; the actual size is
        // negotiated by the XEMBED protocol and the panel's size request.
        req.width = req.height = 1;
        gtk_widget_size_request(socket, &req);
        RET();
    }
error:
    // Either the host didn't pack the socket, or the client window vanished.
    // Clean up: emit removal signal and destroy the socket widget.
    DBG("socket has NO window. destroy it\n");
    // Notify host of removal; the socket was never fully functional.
    g_signal_emit(manager, manager_signals[TRAY_ICON_REMOVED], 0,
        socket);
    // Destroy the socket; this also disconnects all signals connected to it,
    // including "plug_removed" if it was connected above before the goto.
    gtk_widget_destroy(socket);
    RET();
}

/*
 * pending_message_free - free a PendingMessage and its string buffer.
 *
 * Parameters:
 *   message - the PendingMessage to free; must not be NULL
 *
 * Frees message->str first (heap-allocated in handle_begin_message),
 * then frees the PendingMessage struct itself.  Both were g_malloc'd.
 */
static void
pending_message_free (PendingMessage *message)
{
  g_free (message->str);
  g_free (message);
}

/*
 * egg_tray_manager_handle_message_data - process _NET_SYSTEM_TRAY_MESSAGE_DATA.
 *
 * Called when a tray client sends a MESSAGE_DATA ClientMessage carrying up
 * to 20 bytes of balloon message text.  Appends the bytes to the correct
 * PendingMessage (matched by sender window).  When all bytes have arrived
 * (remaining_len == 0), emits "message_sent" and frees the PendingMessage.
 *
 * Parameters:
 *   manager - the EggTrayManager
 *   xevent  - the XClientMessageEvent; window identifies the sender;
 *             data carries up to 20 bytes of message text
 *
 * Protocol detail:
 *   Each MESSAGE_DATA event carries a fixed 20-byte payload regardless of
 *   how much is actually valid.  Only MIN(remaining_len, 20) bytes are
 *   copied into msg->str to avoid buffer overrun.
 *
 * Memory: The PendingMessage is removed from manager->messages and freed
 * via pending_message_free when the message is complete.  The GList node
 * is freed by g_list_remove_link.
 *
 * NOTE: g_list_remove_link removes the GList node but does NOT free it.
 * The node 'p' is leaked here.  Should be g_list_delete_link instead.
 * (This is a minor memory leak: one GList node per completed message.)
 *
 * BUG: memcpy copies from &xevent->data (the XClientMessageEvent data union
 * start) which is correct - the 20 bytes of data.b[] are at that address.
 * However, if the client sends more data events than expected (i.e. sends
 * data after remaining_len has hit 0 from a previous message), the extra
 * events are silently ignored because the PendingMessage has been freed
 * and removed from the list.
 */
static void
egg_tray_manager_handle_message_data (EggTrayManager       *manager,
				       XClientMessageEvent  *xevent)
{
  GList *p;
  int len;

  /* Try to see if we can find the
   * pending message in the list
   */
  for (p = manager->messages; p; p = p->next)
    {
      PendingMessage *msg = p->data;

      if (xevent->window == msg->window)
	{
	  /* Append the message */
          // Clamp to at most 20 bytes (XClientMessageEvent data payload size)
          // and at most the remaining bytes we expect.
	  len = MIN (msg->remaining_len, 20);

          // Append 'len' bytes from the event data into the correct position
          // in msg->str.  The offset (msg->len - msg->remaining_len) gives
          // the number of bytes already received.
	  memcpy ((msg->str + msg->len - msg->remaining_len),
		  &xevent->data, len);
	  msg->remaining_len -= len;

	  if (msg->remaining_len == 0)
	    {
	      GtkSocket *socket;

              // Locate the socket for the window that sent this message
              // using the XID->socket hash table.
	      socket = g_hash_table_lookup (manager->socket_table, GINT_TO_POINTER (msg->window));

	      if (socket)
		{
                  // Full message assembled; notify the host application.
                  // msg->str is NUL-terminated (set at allocation time in
                  // handle_begin_message: msg->str[msg->len] = '\0').
		  g_signal_emit (manager, manager_signals[MESSAGE_SENT], 0,
				 socket, msg->str, msg->id, msg->timeout);
		}
              // Remove this PendingMessage from the in-flight list.
              // g_list_remove_link removes the node but does NOT free it;
              // the node 'p' is a GList cell leak here (minor).
	      manager->messages = g_list_remove_link (manager->messages,
						      p);

	      pending_message_free (msg);
	    }

	  return;
	}
    }
}

/*
 * egg_tray_manager_handle_begin_message - process SYSTEM_TRAY_BEGIN_MESSAGE.
 *
 * Called when a tray client starts a balloon message via opcode 1.
 * Creates a new PendingMessage structure and prepends it to manager->messages.
 * If a message with the same id from the same window is already pending
 * (e.g. client re-sent before completion), the old one is replaced.
 *
 * ClientMessage data layout for BEGIN_MESSAGE:
 *   data.l[0] = timestamp (unused here)
 *   data.l[1] = opcode (SYSTEM_TRAY_BEGIN_MESSAGE = 1)
 *   data.l[2] = timeout in milliseconds
 *   data.l[3] = message length in bytes
 *   data.l[4] = message id (for cancellation)
 *
 * Parameters:
 *   manager - the EggTrayManager
 *   xevent  - the XClientMessageEvent with the begin-message opcode
 *
 * Memory: Allocates a PendingMessage (g_new0) and a string buffer
 * (g_malloc of len+1 bytes).  Both are freed by pending_message_free when
 * the message completes or is cancelled.
 *
 * BUG: If msg->len is 0 (the client sends a zero-length message), g_malloc(1)
 * is called and msg->str[0] = '\0' is set.  Zero subsequent MESSAGE_DATA
 * events will arrive so remaining_len starts at 0 and the message completes
 * immediately in handle_message_data.  However, because no MESSAGE_DATA
 * arrives, the completion code in handle_message_data is NEVER triggered -
 * the zero-length message sits in manager->messages forever (memory leak).
 *
 * BUG: g_list_remove_link in handle_message_data leaks the GList node 'p'.
 * Should use g_list_delete_link.
 */
static void
egg_tray_manager_handle_begin_message (EggTrayManager       *manager,
				       XClientMessageEvent  *xevent)
{
  GList *p;
  PendingMessage *msg;

  /* Check if the same message is
   * already in the queue and remove it if so
   */
  for (p = manager->messages; p; p = p->next)
    {
      PendingMessage *msg = p->data;

      // Match on both window and message id to identify duplicate begins.
      if (xevent->window == msg->window &&
	  xevent->data.l[4] == msg->id)
	{
	  /* Hmm, we found it, now remove it */
          // Free the old PendingMessage and its string buffer.
	  pending_message_free (msg);
          // Remove the GList node; g_list_remove_link does NOT free 'p'.
          // Minor memory leak: the GList node cell is not freed.
	  manager->messages = g_list_remove_link (manager->messages, p);
	  break;
	}
    }

  /* Now add the new message to the queue */
  msg = g_new0 (PendingMessage, 1);
  msg->window = xevent->window;       // X Window ID of the sending tray client
  msg->timeout = xevent->data.l[2];   // requested display duration (ms)
  msg->len = xevent->data.l[3];       // total message byte count
  msg->id = xevent->data.l[4];        // client-assigned message identifier
  msg->remaining_len = msg->len;       // starts equal to total length
  // Allocate buffer with NUL terminator; g_malloc (not g_malloc0) so
  // contents are undefined until filled by MESSAGE_DATA events.
  msg->str = g_malloc (msg->len + 1);
  msg->str[msg->len] = '\0';           // ensure NUL termination up front
  // Prepend is O(1); list order doesn't matter for lookup-by-window.
  manager->messages = g_list_prepend (manager->messages, msg);
}

/*
 * egg_tray_manager_handle_cancel_message - process SYSTEM_TRAY_CANCEL_MESSAGE.
 *
 * Called when a tray client sends opcode 2 to cancel a previously sent
 * balloon message.  Looks up the socket for the sending window and emits
 * "message_cancelled" if found.
 *
 * ClientMessage data layout for CANCEL_MESSAGE:
 *   data.l[0] = timestamp (unused)
 *   data.l[1] = opcode (SYSTEM_TRAY_CANCEL_MESSAGE = 2)
 *   data.l[2] = message id to cancel
 *
 * Parameters:
 *   manager - the EggTrayManager
 *   xevent  - the XClientMessageEvent with the cancel opcode
 *
 * NOTE: This does NOT search and remove any matching PendingMessage from
 * manager->messages.  If the message is still in-flight (not yet fully
 * received), it will continue to accumulate data and eventually emit
 * "message_sent" even though it was supposedly cancelled.  This is a bug.
 */
static void
egg_tray_manager_handle_cancel_message (EggTrayManager       *manager,
					XClientMessageEvent  *xevent)
{
  GtkSocket *socket;

  // Look up the socket that corresponds to the cancelling window.
  socket = g_hash_table_lookup (manager->socket_table, GINT_TO_POINTER (xevent->window));

  if (socket)
    {
      // Notify host; data.l[2] is the message id to cancel.
      g_signal_emit (manager, manager_signals[MESSAGE_CANCELLED], 0,
		     socket, xevent->data.l[2]);
    }
}

/*
 * egg_tray_manager_handle_event - dispatch _NET_SYSTEM_TRAY_OPCODE events.
 *
 * Called from egg_tray_manager_window_filter when a ClientMessage with
 * message_type == opcode_atom is received.  Dispatches based on the opcode
 * stored in data.l[1].
 *
 * Parameters:
 *   manager - the EggTrayManager
 *   xevent  - the XClientMessageEvent to dispatch
 *
 * Returns: GDK_FILTER_REMOVE for all known opcodes (the event is consumed);
 *          GDK_FILTER_CONTINUE for unknown opcodes (passed to other filters).
 *
 * Protocol data layout for all _NET_SYSTEM_TRAY_OPCODE events:
 *   data.l[0] = timestamp
 *   data.l[1] = opcode (0=DOCK, 1=BEGIN_MESSAGE, 2=CANCEL_MESSAGE)
 *   data.l[2..4] = opcode-specific parameters
 */
static GdkFilterReturn
egg_tray_manager_handle_event (EggTrayManager       *manager,
			       XClientMessageEvent  *xevent)
{
  switch (xevent->data.l[1])
    {
    case SYSTEM_TRAY_REQUEST_DOCK:
      egg_tray_manager_handle_dock_request (manager, xevent);
      return GDK_FILTER_REMOVE;    // event consumed; don't pass to GTK

    case SYSTEM_TRAY_BEGIN_MESSAGE:
      egg_tray_manager_handle_begin_message (manager, xevent);
      return GDK_FILTER_REMOVE;    // event consumed

    case SYSTEM_TRAY_CANCEL_MESSAGE:
      egg_tray_manager_handle_cancel_message (manager, xevent);
      return GDK_FILTER_REMOVE;    // event consumed

    default:
      break;
    }

  return GDK_FILTER_CONTINUE;      // unknown opcode; let other filters see it
}

/*
 * egg_tray_manager_window_filter - GDK window filter for the invisible window.
 *
 * This is the core X event dispatcher installed on the invisible window
 * (the selection owner window).  GDK calls this for every X event received
 * on that window before normal GTK processing.
 *
 * Handles two kinds of events:
 *   1. ClientMessage with message_type == opcode_atom:
 *      _NET_SYSTEM_TRAY_OPCODE events (dock, begin_message, cancel_message).
 *   2. ClientMessage with message_type == message_data_atom:
 *      _NET_SYSTEM_TRAY_MESSAGE_DATA events (message content fragments).
 *   3. SelectionClear:
 *      Another process has taken our selection; we emit "lost_selection"
 *      and call unmanage to release resources.
 *
 * Parameters:
 *   xev   - the raw XEvent (GdkXEvent is typedef'd to void *)
 *   event - the translated GdkEvent (may be NULL for non-GDK events)
 *   data  - the EggTrayManager * passed to gdk_window_add_filter
 *
 * Returns: GDK_FILTER_REMOVE if we consumed the event, GDK_FILTER_CONTINUE
 *          to pass it on to GTK's normal event processing.
 *
 * XEMBED note: XEMBED protocol messages sent to/from the plug window are
 * handled by GtkSocket internally and do NOT appear here (they go to the
 * socket's window filter).
 *
 * Race condition: Between receiving SelectionClear and calling unmanage(),
 * another dock request might arrive (from a client that received our MANAGER
 * broadcast but before we release the selection).  This is handled gracefully
 * because the filter is removed in unmanage().
 */
static GdkFilterReturn
egg_tray_manager_window_filter (GdkXEvent *xev, GdkEvent *event, gpointer data)
{
  XEvent *xevent = (GdkXEvent *)xev;
  EggTrayManager *manager = data;

  if (xevent->type == ClientMessage)
    {
      if (xevent->xclient.message_type == manager->opcode_atom)
	{
          // _NET_SYSTEM_TRAY_OPCODE: dock request, begin message, or cancel.
	  return egg_tray_manager_handle_event (manager, (XClientMessageEvent *)xevent);
	}
      else if (xevent->xclient.message_type == manager->message_data_atom)
	{
          // _NET_SYSTEM_TRAY_MESSAGE_DATA: fragment of an in-flight balloon.
	  egg_tray_manager_handle_message_data (manager, (XClientMessageEvent *)xevent);
	  return GDK_FILTER_REMOVE;
	}
    }
  else if (xevent->type == SelectionClear)
    {
      // Another process has claimed _NET_SYSTEM_TRAY_S<n>; we must yield.
      // Emit "lost_selection" so the host can react (e.g. show an error).
      g_signal_emit (manager, manager_signals[LOST_SELECTION], 0);
      // Release all X11 resources; this also removes this filter callback.
      egg_tray_manager_unmanage (manager);
    }

  return GDK_FILTER_CONTINUE;
}

/*
 * egg_tray_manager_unmanage - release X11 selection and all managed resources.
 *
 * Called from finalize (when the last ref is dropped) and from the window
 * filter when a SelectionClear is received.  Must be idempotent (safe to
 * call when already unmanaged) because it can be triggered from multiple paths.
 *
 * Actions:
 *   1. If we still own the X11 selection, release it by calling
 *      XSetSelectionOwner with owner=None and a fresh server timestamp.
 *   2. Remove the GDK window filter from the invisible window.
 *   3. Set manager->invisible to NULL (prevents re-entry).
 *   4. Destroy the invisible window and drop the extra GObject reference
 *      that was taken in egg_tray_manager_manage_xscreen.
 *
 * Parameters:
 *   manager - the EggTrayManager to unmanage
 *
 * NOTE: The socket_table and pending messages are NOT cleaned up here.
 * Existing GtkSocket widgets remain in their containers (they are owned
 * by the host's widget hierarchy) but the socket_table hash still references
 * them.  The hash table is created in init but never destroyed in finalize -
 * this is a memory leak (the hash table itself is not freed).
 *
 * NOTE: Pending messages (manager->messages GList) are not freed here;
 * this is a memory leak if there are in-flight balloon messages when
 * unmanage is called.
 *
 * GTK2 note: GTK_WIDGET_REALIZED() is a deprecated macro in GTK2 and
 * does not exist in GTK3; use gtk_widget_get_realized() instead.
 */
static void
egg_tray_manager_unmanage (EggTrayManager *manager)
{
  Display *display;
  guint32 timestamp;
  GtkWidget *invisible;

  // Idempotency check: if invisible is NULL we're already unmanaged.
  if (manager->invisible == NULL)
    return;

  invisible = manager->invisible;
  // These asserts verify the invariant that invisible is a realized GtkInvisible.
  g_assert (GTK_IS_INVISIBLE (invisible));
  g_assert (GTK_WIDGET_REALIZED (invisible));
  g_assert (GDK_IS_WINDOW (invisible->window));

  display = GDK_WINDOW_XDISPLAY (invisible->window);

  // Only release the selection if we currently own it.  Another process might
  // have already stolen it (SelectionClear path), in which case XGetSelectionOwner
  // returns a different window and we skip the XSetSelectionOwner call.
  if (XGetSelectionOwner (display, manager->selection_atom) ==
      GDK_WINDOW_XWINDOW (invisible->window))
    {
      // Use a fresh server timestamp to satisfy the selection protocol.
      timestamp = gdk_x11_get_server_time (invisible->window);
      // Release the selection by setting owner to None.
      XSetSelectionOwner (display, manager->selection_atom, None, timestamp);
    }

  // Remove our GDK window filter; after this, no more X events will be
  // routed to egg_tray_manager_window_filter.
  gdk_window_remove_filter (invisible->window, egg_tray_manager_window_filter, manager);

  manager->invisible = NULL; /* prior to destroy for reentrancy paranoia */
  // Destroy the GTK widget (destroys the underlying X window).
  gtk_widget_destroy (invisible);
  // Release the extra GObject reference we took in manage_xscreen to keep
  // the invisible widget alive independently of any parent container.
  g_object_unref (G_OBJECT (invisible));
}

/*
 * egg_tray_manager_manage_xscreen - low-level screen management on a Screen *.
 *
 * Acquires the _NET_SYSTEM_TRAY_S<n> X11 selection for the given Xlib Screen,
 * creates the invisible selection-owner window, broadcasts the MANAGER
 * announcement, and installs the GDK window filter.
 *
 * Parameters:
 *   manager - the EggTrayManager (must not already be managing a screen;
 *             checked by manager->screen == NULL guard - BUT screen is never
 *             assigned, so this guard is effectively non-functional)
 *   xscreen - the Xlib Screen * to manage
 *
 * Returns: TRUE on success (selection acquired), FALSE if selection acquisition
 *          failed (another manager already owns it).
 *
 * Selection ownership protocol:
 *   1. Create and realize a GtkInvisible on the correct GdkScreen.
 *   2. Intern "_NET_SYSTEM_TRAY_S<n>" atom.
 *   3. XSetSelectionOwner with a fresh server timestamp.
 *   4. Verify ownership with XGetSelectionOwner (TOCTOU race possible).
 *   5. If owned, broadcast MANAGER ClientMessage to root window with
 *      StructureNotifyMask so all clients receive it.
 *   6. Intern opcode and message_data atoms.
 *   7. Install GDK window filter on the invisible window.
 *
 * Note: The check `#if 0 ... egg_tray_manager_check_running_xscreen` is
 * disabled, leaving a comment explaining the intent but allowing the caller
 * (egg_tray_manager_manage_screen) to proceed even if another manager is
 * running.  The actual ownership verification happens at step 4.
 *
 * Memory: invisible is g_object_ref'd after creation (refcount becomes 2);
 * the extra ref is dropped in egg_tray_manager_unmanage.
 *
 * BUG: manager->screen is never assigned in this function; the precondition
 * `g_return_val_if_fail(manager->screen == NULL, FALSE)` in manage_screen
 * always passes (since screen starts NULL and is never set).  Calling
 * manage_screen twice on the same manager is NOT prevented.
 */
static gboolean
egg_tray_manager_manage_xscreen (EggTrayManager *manager, Screen *xscreen)
{
  GtkWidget *invisible;
  char *selection_atom_name;
  guint32 timestamp;
  GdkScreen *screen;

  g_return_val_if_fail (EGG_IS_TRAY_MANAGER (manager), FALSE);
  // This guard is meant to prevent managing a screen twice, but since
  // manager->screen is never assigned anywhere, it always passes.
  g_return_val_if_fail (manager->screen == NULL, FALSE);

  /* If there's already a manager running on the screen
   * we can't create another one.
   */
#if 0
  /* This check is disabled; the actual ownership check is done via
   * XGetSelectionOwner after XSetSelectionOwner below. */
  if (egg_tray_manager_check_running_xscreen (xscreen))
    return FALSE;
#endif

  // Convert the Xlib Screen * to a GdkScreen * via the display.
  screen = gdk_display_get_screen (gdk_x11_lookup_xdisplay (DisplayOfScreen (xscreen)),
				   XScreenNumberOfScreen (xscreen));

  // Create an off-screen window to act as the selection owner.
  // GtkInvisible has no visible window but has a valid GdkWindow/XWindow.
  invisible = gtk_invisible_new_for_screen (screen);
  // Realize immediately so invisible->window is valid for XSetSelectionOwner.
  gtk_widget_realize (invisible);

  // Request property-change and structure events on the invisible window.
  // PROPERTY_CHANGE_MASK is needed for gdk_x11_get_server_time (which works
  // by setting a dummy property and reading the timestamp from the PropertyNotify).
  gtk_widget_add_events (invisible, GDK_PROPERTY_CHANGE_MASK | GDK_STRUCTURE_MASK);

  // Build the screen-specific selection atom name: "_NET_SYSTEM_TRAY_S0" etc.
  selection_atom_name = g_strdup_printf ("_NET_SYSTEM_TRAY_S%d",
					 XScreenNumberOfScreen (xscreen));
  // Intern the atom with False (create if not already exists).
  manager->selection_atom = XInternAtom (DisplayOfScreen (xscreen), selection_atom_name, False);

  g_free (selection_atom_name);

  // Obtain a current server timestamp by triggering a PropertyNotify event.
  // Using a current timestamp prevents old selection requests from being honoured.
  timestamp = gdk_x11_get_server_time (invisible->window);
  // Attempt to take ownership of the tray selection.
  XSetSelectionOwner (DisplayOfScreen (xscreen), manager->selection_atom,
		      GDK_WINDOW_XWINDOW (invisible->window), timestamp);

  /* Check if we were could set the selection owner successfully */
  // Verify we are now the selection owner (another process could have won
  // the race between XSetSelectionOwner and XGetSelectionOwner).
  if (XGetSelectionOwner (DisplayOfScreen (xscreen), manager->selection_atom) ==
      GDK_WINDOW_XWINDOW (invisible->window))
    {
      XClientMessageEvent xev;

      // Broadcast MANAGER announcement to the root window so that tray
      // clients waiting for a manager (via SubstructureNotifyMask/StructureNotifyMask)
      // will know they can now send dock requests.
      xev.type = ClientMessage;
      xev.window = RootWindowOfScreen (xscreen);
      xev.message_type = XInternAtom (DisplayOfScreen (xscreen), "MANAGER", False);

      xev.format = 32;            // data is in 32-bit long format
      xev.data.l[0] = timestamp;  // time of selection acquisition
      xev.data.l[1] = manager->selection_atom;  // which selection was acquired
      xev.data.l[2] = GDK_WINDOW_XWINDOW (invisible->window);  // owner window XID
      xev.data.l[3] = 0;	/* manager specific data */
      xev.data.l[4] = 0;	/* manager specific data */

      // Send to root window; StructureNotifyMask reaches all clients listening
      // on the root for structure events (standard for MANAGER broadcasts).
      XSendEvent (DisplayOfScreen (xscreen),
		  RootWindowOfScreen (xscreen),
		  False, StructureNotifyMask, (XEvent *)&xev);

      manager->invisible = invisible;
      // Take an extra reference to keep the invisible widget alive even if
      // it gets unparented; balanced by g_object_unref in egg_tray_manager_unmanage.
      g_object_ref (G_OBJECT (manager->invisible));

      // Intern _NET_SYSTEM_TRAY_OPCODE atom for identifying dock/message events.
      manager->opcode_atom = XInternAtom (DisplayOfScreen (xscreen),
					  "_NET_SYSTEM_TRAY_OPCODE",
					  False);

      // Intern _NET_SYSTEM_TRAY_MESSAGE_DATA atom for balloon message fragments.
      manager->message_data_atom = XInternAtom (DisplayOfScreen (xscreen),
						"_NET_SYSTEM_TRAY_MESSAGE_DATA",
						False);

      /* Add a window filter */
      // Install the X event filter on the invisible window's GdkWindow.
      // All X events for this window will be passed through our filter first.
      gdk_window_add_filter (invisible->window, egg_tray_manager_window_filter, manager);
      return TRUE;
    }
  else
    {
      // We did not win the selection race; destroy the invisible window.
      gtk_widget_destroy (invisible);

      return FALSE;
    }
}

/*
 * egg_tray_manager_manage_screen - public API to begin managing a GdkScreen.
 *
 * Thin wrapper around egg_tray_manager_manage_xscreen that converts the
 * GdkScreen to the underlying Xlib Screen *.
 *
 * Parameters:
 *   manager - the EggTrayManager; must not be NULL and must not already
 *             be managing a screen (manager->screen == NULL - but see BUG)
 *   screen  - the GdkScreen to manage; must be a valid GDK X11 screen
 *
 * Returns: TRUE if the selection was acquired and management started,
 *          FALSE otherwise.
 *
 * BUG: manager->screen is checked (== NULL) but never set, so the guard
 * g_return_val_if_fail(manager->screen == NULL) is always TRUE and provides
 * no protection against double-managing.
 */
gboolean
egg_tray_manager_manage_screen (EggTrayManager *manager,
				GdkScreen      *screen)
{
  g_return_val_if_fail (GDK_IS_SCREEN (screen), FALSE);
  // This guard is meant to prevent re-managing the same manager but is
  // ineffective because manager->screen is never set (see above).
  g_return_val_if_fail (manager->screen == NULL, FALSE);

  return egg_tray_manager_manage_xscreen (manager,
					  GDK_SCREEN_XSCREEN (screen));
}

/*
 * egg_tray_manager_check_running_xscreen - test if a tray manager already
 * owns the selection on the given Xlib Screen.
 *
 * Parameters:
 *   xscreen - the Xlib Screen to check
 *
 * Returns: TRUE if _NET_SYSTEM_TRAY_S<n> has an owner, FALSE otherwise.
 *
 * NOTE: This is subject to a TOCTOU race.  The owner could exit between
 * this check and a subsequent manage_screen call.  Callers should handle
 * manage_screen returning FALSE gracefully.
 *
 * The atom is interned with False (create-if-needed); XInternAtom may
 * perform a round-trip X request on first call for each screen.
 */
static gboolean
egg_tray_manager_check_running_xscreen (Screen *xscreen)
{
  Atom selection_atom;
  char *selection_atom_name;

  selection_atom_name = g_strdup_printf ("_NET_SYSTEM_TRAY_S%d",
					 XScreenNumberOfScreen (xscreen));
  selection_atom = XInternAtom (DisplayOfScreen (xscreen), selection_atom_name, False);
  g_free (selection_atom_name);

  // XGetSelectionOwner returns None (0) if no owner, non-zero XID otherwise.
  if (XGetSelectionOwner (DisplayOfScreen (xscreen), selection_atom))
    return TRUE;
  else
    return FALSE;
}

/*
 * egg_tray_manager_check_running - public API to test if a tray manager is
 * already running on the given GdkScreen.
 *
 * Parameters:
 *   screen - must not be NULL and must be a valid GdkScreen
 *
 * Returns: TRUE if another tray manager holds the selection, FALSE otherwise.
 */
gboolean
egg_tray_manager_check_running (GdkScreen *screen)
{
  g_return_val_if_fail (GDK_IS_SCREEN (screen), FALSE);

  return egg_tray_manager_check_running_xscreen (GDK_SCREEN_XSCREEN (screen));
}

/*
 * egg_tray_manager_get_child_title - read _NET_WM_NAME from a tray icon window.
 *
 * Fetches the UTF-8 window title from the embedded client's X window using
 * XGetWindowProperty with the _NET_WM_NAME atom (UTF8_STRING type).
 *
 * Parameters:
 *   manager - the active EggTrayManager; used for type-check only
 *   child   - a GtkSocket * (EggTrayManagerChild *) that was previously
 *             delivered via "tray_icon_added"; must have "egg-tray-child-window"
 *             object data set (a heap-allocated Window *)
 *
 * Returns: a newly g_malloc'd NUL-terminated UTF-8 string, or NULL if:
 *   - the _NET_WM_NAME property does not exist or is not UTF8_STRING type
 *   - an X error occurred (client window was destroyed)
 *   - the property data is not valid UTF-8 (g_utf8_validate fails)
 *   - nitems == 0 (empty property)
 *
 * Memory ownership: Caller owns the returned string; must free with g_free().
 *   Internally, XGetWindowProperty allocates 'val' via XMalloc; it is freed
 *   with XFree() before return.  g_strndup creates the copy returned to the
 *   caller.
 *
 * Error handling: gdk_error_trap_push/pop wraps XGetWindowProperty to
 * catch BadWindow errors from recently-destroyed client windows.
 *
 * Deprecated API: GDK_DISPLAY() is a GTK2 macro; GTK3 uses
 * gdk_display_get_default() or gdk_window_get_display().
 *
 * BUG: If child_window is NULL (e.g. called after plug_removed cleared
 * the data), dereferencing it (*child_window) is undefined behaviour.
 * There is no NULL guard on child_window before the dereference.
 */
char *
egg_tray_manager_get_child_title (EggTrayManager *manager,
				  EggTrayManagerChild *child)
{
  Window *child_window;
  Atom utf8_string, atom, type;
  int result;
  gchar *val, *retval;
  int format;
  gulong nitems;
  gulong bytes_after;
  guchar *tmp = NULL;

  g_return_val_if_fail (EGG_IS_TRAY_MANAGER (manager), NULL);
  g_return_val_if_fail (GTK_IS_SOCKET (child), NULL);

  // Retrieve the client's X Window ID stored during dock request handling.
  // OWNERSHIP: the Window * is owned by the GObject data system; do not free.
  child_window = g_object_get_data (G_OBJECT (child),
        "egg-tray-child-window");
  // BUG: no NULL check on child_window; if it was cleared by plug_removed,
  // the dereference below is undefined behaviour.

  // Intern the UTF8_STRING type atom and _NET_WM_NAME property atom.
  // GDK_DISPLAY() is deprecated; use gdk_display_get_default() in GTK3.
  utf8_string = XInternAtom (GDK_DISPLAY (), "UTF8_STRING", False);
  atom = XInternAtom (GDK_DISPLAY (), "_NET_WM_NAME", False);

  // Push an X error trap so BadWindow errors from dead client windows
  // don't abort the process.
  gdk_error_trap_push();

  // Read the _NET_WM_NAME property from the client's X window.
  // offset=0, length=G_MAXLONG (read all), delete=False, req_type=utf8_string.
  // On success, 'tmp' points to XMalloc'd data; must be freed with XFree.
  result = XGetWindowProperty (GDK_DISPLAY (), *child_window, atom, 0,
        G_MAXLONG, False, utf8_string, &type, &format, &nitems,
        &bytes_after, &tmp);
  val = (gchar *) tmp;

  // Pop the error trap; if non-zero, an X error occurred.
  if (gdk_error_trap_pop() || result != Success || type != utf8_string)
    return NULL;  // val may be non-NULL but we can't trust it; XFree not called here - minor leak

  // Validate format (8-bit chars) and non-empty content.
  if (format != 8 || nitems == 0) {
      if (val)
          XFree (val);
      return NULL;
  }

  // Verify the raw bytes are valid UTF-8 before converting to GLib string.
  if (!g_utf8_validate (val, nitems, NULL))
    {
      XFree (val);
      return NULL;
    }

  // Copy nitems bytes from the X-allocated buffer into a GLib-owned string.
  retval = g_strndup (val, nitems);

  // Free the X server's copy of the property data.
  XFree (val);

  return retval;  // caller must g_free() this

}
