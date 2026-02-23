/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* eggtraymanager.h
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
 * EggTrayManager - freedesktop.org System Tray Protocol manager
 *
 * This header declares the EggTrayManager GObject class (originally from
 * GNOME's libegg library, vendored into fbpanel).  EggTrayManager implements
 * the host side of the freedesktop.org System Tray Protocol Specification
 * (https://specifications.freedesktop.org/systemtray-spec/latest/).
 *
 * Protocol overview:
 *   1. The tray manager acquires the _NET_SYSTEM_TRAY_S<n> X11 selection
 *      (where <n> is the screen number).  Only one manager may hold this
 *      selection at a time; a race is possible if two processes both attempt
 *      to acquire it simultaneously.
 *   2. After acquiring the selection the manager broadcasts a MANAGER
 *      ClientMessage to the root window so waiting tray clients know a
 *      manager is now available.
 *   3. Tray clients send _NET_SYSTEM_TRAY_OPCODE ClientMessages with opcode
 *      SYSTEM_TRAY_REQUEST_DOCK to request embedding.
 *   4. The manager creates a GtkSocket, calls gtk_socket_add_id() with the
 *      client's X window, and emits "tray_icon_added".
 *   5. XEMBED protocol (XEmbed spec) handles the actual window reparenting
 *      and focus/activity negotiation between the socket and plug windows.
 *   6. Balloon messages are sent via SYSTEM_TRAY_BEGIN_MESSAGE followed by
 *      _NET_SYSTEM_TRAY_MESSAGE_DATA ClientMessages carrying up to 20 bytes
 *      per message until the full string is transmitted.
 *
 * GTK2/GTK3 compatibility note:
 *   This file uses GTK_TYPE_SOCKET in signal type registration and the raw
 *   GdkWindow pointer via widget->window.  Both are GTK2 APIs that were
 *   removed in GTK3.  Porting to GTK3 requires gtk_widget_get_window() and
 *   other accessor functions.
 *
 * Thread safety: EggTrayManager is NOT thread-safe.  All operations must
 * occur on the GLib main thread that drives the GDK event loop.
 */

#ifndef __EGG_TRAY_MANAGER_H__
#define __EGG_TRAY_MANAGER_H__

#include <gtk/gtkwidget.h>
#include <gdk/gdkx.h>

G_BEGIN_DECLS

/* GObject type system macros for EggTrayManager. */
#define EGG_TYPE_TRAY_MANAGER			(egg_tray_manager_get_type ())
#define EGG_TRAY_MANAGER(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), EGG_TYPE_TRAY_MANAGER, EggTrayManager))
#define EGG_TRAY_MANAGER_CLASS(klass)		(G_TYPE_CHECK_CLASS_CAST ((klass), EGG_TYPE_TRAY_MANAGER, EggTrayManagerClass))
#define EGG_IS_TRAY_MANAGER(obj)		(G_TYPE_CHECK_INSTANCE_TYPE ((obj), EGG_TYPE_TRAY_MANAGER))
#define EGG_IS_TRAY_MANAGER_CLASS(klass)	(G_TYPE_CHECK_CLASS_TYPE ((klass), EGG_TYPE_TRAY_MANAGER))
#define EGG_TRAY_MANAGER_GET_CLASS(obj)		(G_TYPE_INSTANCE_GET_CLASS ((obj), EGG_TYPE_TRAY_MANAGER, EggTrayManagerClass))

/* Forward declarations */
typedef struct _EggTrayManager	     EggTrayManager;
typedef struct _EggTrayManagerClass  EggTrayManagerClass;

/*
 * EggTrayManagerChild - opaque type alias for GtkSocket used in signal
 * signatures to indicate a tray icon's container widget.
 *
 * In practice, every EggTrayManagerChild * passed in signal emissions is
 * actually a GtkSocket *.  Callers can safely cast to GtkSocket * and use
 * it with GTK socket APIs.  The "egg-tray-child-window" object data key
 * on the socket stores a heap-allocated (Window *) for the embedded X window.
 *
 * Memory: The Window * stored as object data is owned by the GObject data
 * system and freed via g_free() (registered as the destroy notify in
 * g_object_set_data_full).  Do NOT free it manually.
 */
typedef struct _EggTrayManagerChild  EggTrayManagerChild;

/*
 * struct _EggTrayManager - instance structure
 *
 * Fields:
 *   parent_instance - GObject base; must be first member
 *
 *   opcode_atom      - Atom for "_NET_SYSTEM_TRAY_OPCODE"; used to identify
 *                      incoming dock/message ClientMessage events
 *   selection_atom   - Atom for "_NET_SYSTEM_TRAY_S<n>" selection; held as
 *                      long as this manager owns the tray selection
 *   message_data_atom - Atom for "_NET_SYSTEM_TRAY_MESSAGE_DATA"; used to
 *                       identify incoming balloon message data ClientMessages
 *
 *   invisible  - GtkInvisible widget whose underlying X window is used as
 *                the selection owner window.  Holds one extra GObject ref
 *                (see egg_tray_manager_manage_xscreen).  NULL after unmanage.
 *   screen     - the GdkScreen being managed; NULL until manage_screen() is
 *                called.  Currently set in egg_tray_manager_manage_xscreen
 *                but never actually assigned to manager->screen (BUG).
 *
 *   messages     - GList of PendingMessage * for in-flight balloon messages;
 *                  each entry is freed when all data arrives or on unmanage
 *   socket_table - GHashTable mapping (Window XID -> GtkSocket *) for all
 *                  currently embedded tray icons.  Key is GINT_TO_POINTER(xid),
 *                  value is a borrowed GtkSocket * (owned by the container).
 *                  NULL destroy notify - entries are removed when the plug
 *                  is withdrawn (egg_tray_manager_plug_removed).
 */
struct _EggTrayManager
{
  GObject parent_instance;

  Atom opcode_atom;
  Atom selection_atom;
  Atom message_data_atom;

  GtkWidget *invisible;
  GdkScreen *screen;

  GList *messages;
  GHashTable *socket_table;
};

/*
 * struct _EggTrayManagerClass - class (vtable) structure
 *
 * Signals (all have G_SIGNAL_RUN_LAST; connect with g_signal_connect):
 *
 *   tray_icon_added(manager, child):
 *     Emitted when a tray client has been successfully embedded.
 *     'child' is a GtkSocket * ready for packing into a container.
 *     The socket is shown (gtk_widget_show was called) before emission.
 *     Receivers MUST add the socket to a container; if the socket has no
 *     toplevel window after the signal, the icon is destroyed.
 *     Memory: socket ownership passes to whichever container packs it.
 *
 *   tray_icon_removed(manager, child):
 *     Emitted when a tray client's plug window has been removed (client exit,
 *     crash, or explicit undock).  The socket is in the process of being
 *     destroyed (plug_removed returned FALSE).  Receivers MUST NOT attempt
 *     to re-parent or use the socket widget after this signal.
 *
 *   message_sent(manager, child, message, id, timeout):
 *     Emitted when a complete balloon message has been assembled from
 *     SYSTEM_TRAY_BEGIN_MESSAGE + MESSAGE_DATA events.
 *     'message' is a NUL-terminated UTF-8 string, valid only for the
 *     duration of the signal emission; receivers must copy it if they
 *     need it later.  'id' identifies the message for later cancellation.
 *     'timeout' is the requested display time in milliseconds (may be 0
 *     for "show until dismissed").
 *
 *   message_cancelled(manager, child, id):
 *     Emitted when a tray client sends SYSTEM_TRAY_CANCEL_MESSAGE for a
 *     previously sent balloon.  Receivers should hide/discard the message
 *     matching 'id'.  NOTE: currently not implemented in main.c.
 *
 *   lost_selection(manager):
 *     Emitted when another process has taken the _NET_SYSTEM_TRAY_S<n>
 *     selection (SelectionClear X event).  After this signal the manager
 *     calls egg_tray_manager_unmanage() and stops accepting new icons.
 *     Existing embedded sockets are NOT automatically removed; callers
 *     may need to clean up manually.
 */
struct _EggTrayManagerClass
{
  GObjectClass parent_class;

  void (* tray_icon_added)   (EggTrayManager      *manager,
			      EggTrayManagerChild *child);
  void (* tray_icon_removed) (EggTrayManager      *manager,
			      EggTrayManagerChild *child);

  void (* message_sent)      (EggTrayManager      *manager,
			      EggTrayManagerChild *child,
			      const gchar         *message,
			      glong                id,
			      glong                timeout);

  void (* message_cancelled) (EggTrayManager      *manager,
			      EggTrayManagerChild *child,
			      glong                id);

  void (* lost_selection)    (EggTrayManager      *manager);
};

/*
 * egg_tray_manager_get_type - return the GType for EggTrayManager.
 *
 * Lazily registers the type on first call.  Thread-unsafe (no locking on
 * the static GType variable), but GLib's type system is used exclusively
 * from the main thread in this codebase.
 *
 * Returns: GType identifier for EggTrayManager.
 */
GType           egg_tray_manager_get_type        (void);

/*
 * egg_tray_manager_check_running - test whether a tray manager already owns
 * the selection on the given screen.
 *
 * Parameters:
 *   screen - the GdkScreen to check; must not be NULL
 *
 * Returns: TRUE if a tray manager already holds _NET_SYSTEM_TRAY_S<n>,
 *          FALSE otherwise.
 *
 * NOTE: There is a TOCTOU (time-of-check/time-of-use) race between this call
 * and egg_tray_manager_manage_screen(): another process could acquire the
 * selection in the window between the two calls.
 */
gboolean        egg_tray_manager_check_running   (GdkScreen           *screen);

/*
 * egg_tray_manager_new - allocate a new EggTrayManager instance.
 *
 * Returns: a new EggTrayManager with refcount=1.  The caller owns the
 *          reference and must call g_object_unref() when done.
 *
 * The manager is not yet associated with any screen; call
 * egg_tray_manager_manage_screen() to start managing a screen.
 */
EggTrayManager *egg_tray_manager_new             (void);

/*
 * egg_tray_manager_manage_screen - acquire the system tray selection and
 * begin managing the given screen.
 *
 * Parameters:
 *   manager - the EggTrayManager (must not already be managing a screen)
 *   screen  - the GdkScreen to manage; must not be NULL
 *
 * Returns: TRUE if the selection was successfully acquired and the manager
 *          is now active; FALSE if another manager owns the selection.
 *
 * On success:
 *   - Creates a GtkInvisible window as the selection owner
 *   - Broadcasts a MANAGER ClientMessage to notify waiting tray clients
 *   - Installs a GDK window filter to receive ClientMessage events
 *   - Interns the _NET_SYSTEM_TRAY_OPCODE and _NET_SYSTEM_TRAY_MESSAGE_DATA
 *     atoms for later ClientMessage identification
 *
 * NOTE: manager->screen is never assigned (BUG in implementation); the
 * precondition check `manager->screen == NULL` in egg_tray_manager_manage_screen
 * will always pass on a fresh manager, but the field is never set, so calling
 * manage_screen() twice on the same manager will not be prevented by the guard.
 */
gboolean        egg_tray_manager_manage_screen   (EggTrayManager      *manager,
						  GdkScreen           *screen);

/*
 * egg_tray_manager_get_child_title - fetch the _NET_WM_NAME of a tray icon.
 *
 * Reads the _NET_WM_NAME property (UTF8_STRING type) from the embedded
 * client X window associated with the given socket/child.
 *
 * Parameters:
 *   manager - the active EggTrayManager; must not be NULL
 *   child   - a GtkSocket * previously delivered via "tray_icon_added";
 *             must have "egg-tray-child-window" object data set
 *
 * Returns: a newly-allocated NUL-terminated UTF-8 string containing the
 *          window title, or NULL if the property is absent, not UTF-8, or
 *          an X error occurred.
 *
 * Memory: The returned string is owned by the caller and must be freed with
 *         g_free().
 *
 * Error handling: An X error trap (gdk_error_trap_push/pop) protects the
 * XGetWindowProperty call in case the client window has already been destroyed.
 */
char           *egg_tray_manager_get_child_title (EggTrayManager      *manager,
						  EggTrayManagerChild *child);

G_END_DECLS

#endif /* __EGG_TRAY_MANAGER_H__ */
