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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
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
 * gtkbgbox.h -- Public interface for GtkBgbox, a GtkBin subclass that adds
 * support for pseudo-transparent and tinted backgrounds on the fbpanel desktop
 * panel.
 *
 * GtkBgbox extends GtkBin (which itself extends GtkContainer -> GtkWidget ->
 * GObject).  It owns its own GdkWindow (unlike GtkEventBox in NO_WINDOW mode),
 * which allows it to set a background pixmap directly on that window.
 *
 * Three background modes are provided:
 *   BG_NONE    - no explicit background set yet (initial state).
 *   BG_STYLE   - use the GTK theme style for this widget's state
 *                (the default after realize if BG_NONE is still set).
 *   BG_ROOT    - sample a region of the X root-window pixmap, optionally
 *                tinted/composited, and use that as the window background.
 *                This creates the "pseudo-transparency" effect common in
 *                desktop panels.
 *   BG_INHERIT - set the GDK window background to NULL/parent, telling X
 *                to inherit/expose the parent window's background; useful
 *                for compositing with the parent panel background.
 *
 * The private state (pixmap, FbBg handle, signal ID, etc.) is stored in
 * GtkBgboxPrivate, allocated by GObject via G_ADD_PRIVATE().
 *
 * Ownership / ref-count summary:
 *   - The GtkBgbox instance is a normal GTK widget; callers obtain a floating
 *     reference from gtk_bgbox_new() that is sunk when the widget is packed
 *     into a container.
 *   - priv->bg  : FbBg singleton obtained via fb_bg_get_for_display().
 *                 GtkBgbox holds one reference (g_object_ref'd internally).
 *                 Released in gtk_bgbox_finalize().
 *   - priv->pixmap : GdkPixmap allocated by fb_bg_get_xroot_pix_for_win().
 *                    GtkBgbox owns this reference; released before each new
 *                    allocation and in gtk_bgbox_finalize().
 */

#ifndef __GTK_BGBOX_H__
#define __GTK_BGBOX_H__


#include <gdk/gdk.h>
#include <gtk/gtkbin.h>
#include "bg.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* -------------------------------------------------------------------------
 * GObject type macros
 *
 * These follow the standard GTK/GObject naming convention:
 *   GTK_TYPE_BGBOX      - the GType value for run-time type identification.
 *   GTK_BGBOX(obj)      - cast obj to GtkBgbox*; fails loudly in debug builds.
 *   GTK_BGBOX_CLASS(k)  - cast k to GtkBgboxClass*.
 *   GTK_IS_BGBOX(obj)   - boolean type check; safe to call with NULL.
 *   GTK_IS_BGBOX_CLASS  - boolean class type check.
 *   GTK_BGBOX_GET_CLASS - retrieve the class vtable pointer from an instance.
 * ------------------------------------------------------------------------- */
#define GTK_TYPE_BGBOX              (gtk_bgbox_get_type ())
#define GTK_BGBOX(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_BGBOX, GtkBgbox))
#define GTK_BGBOX_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_BGBOX, GtkBgboxClass))
#define GTK_IS_BGBOX(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_BGBOX))
#define GTK_IS_BGBOX_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_BGBOX))
#define GTK_BGBOX_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_BGBOX, GtkBgboxClass))

/* Forward declarations for the instance and class structs. */
typedef struct _GtkBgbox	  GtkBgbox;
typedef struct _GtkBgboxClass  GtkBgboxClass;

/*
 * GtkBgbox instance structure.
 *
 * The public part contains only the inherited GtkBin member.  All
 * GtkBgbox-specific fields live in GtkBgboxPrivate (accessed via
 * gtk_bgbox_get_instance_private()), which is allocated inline after this
 * struct by the GObject machinery when G_ADD_PRIVATE() is used.
 */
struct _GtkBgbox
{
    GtkBin bin;  // must be first: allows safe casting between GtkBgbox* and GtkBin*/GtkWidget*
};

/*
 * GtkBgbox class structure.
 *
 * Only stores the parent class vtable.  No additional virtual methods are
 * defined by GtkBgbox itself.
 */
struct _GtkBgboxClass
{
    GtkBinClass parent_class;  // inherits all GtkBin/GtkContainer/GtkWidget vfuncs
};

/*
 * Background mode enumeration.
 *
 * BG_NONE    (0) - unset; treated as BG_STYLE after realise.
 * BG_STYLE   (1) - use GTK theme/style background (gtk_style_set_background).
 * BG_ROOT    (2) - sample the X root window pixmap (pseudo-transparency),
 *                  optionally tinted via fb_bg_composite().
 * BG_INHERIT (3) - inherit background from parent window (gdk_window_set_back_pixmap NULL/TRUE).
 * BG_LAST    (4) - sentinel; not a valid mode.
 */
enum { BG_NONE, BG_STYLE, BG_ROOT, BG_INHERIT, BG_LAST };

/*
 * gtk_bgbox_get_type:
 *
 * Returns the GType for GtkBgbox, registering it with the GObject type system
 * the first time it is called.  Marked G_GNUC_CONST because the return value
 * never changes after the first call.
 */
GType	   gtk_bgbox_get_type (void) G_GNUC_CONST;

/*
 * gtk_bgbox_new:
 *
 * Allocates and initialises a new GtkBgbox widget.
 *
 * Returns: a floating GtkWidget* reference.  The caller does NOT own a full
 *          reference until the widget is added to a container (which sinks
 *          the floating ref).  If the widget is never added to a container,
 *          the caller must g_object_ref_sink() it and later g_object_unref().
 */
GtkWidget* gtk_bgbox_new (void);

/*
 * gtk_bgbox_set_background:
 *
 * Changes the background rendering mode for widget (which must be a GtkBgbox).
 *
 * Parameters:
 *   widget    - the GtkBgbox whose background is being configured.
 *               Silently returns if GTK_IS_BGBOX(widget) is FALSE.
 *   bg_type   - one of the BG_* enum values (BG_NONE, BG_STYLE, BG_ROOT,
 *               BG_INHERIT).
 *   tintcolor - 32-bit ARGB tint colour; only used when bg_type == BG_ROOT.
 *               The upper 8 bits are ignored; only the lower 24 (RGB) are used
 *               by fb_bg_composite().
 *   alpha     - opacity of the tint overlay in the range [0, 255].
 *               0 means no tint; 255 means fully opaque tint.
 *               Only used when bg_type == BG_ROOT.
 *
 * Side effects:
 *   - Drops and re-creates priv->pixmap.
 *   - May acquire or release priv->bg (FbBg singleton ref) and priv->sid
 *     (the "changed" signal connection).
 *   - Calls gtk_widget_queue_draw() unconditionally to force a repaint.
 *   - Emits a "style" property-change notification on the widget.
 *
 * Thread safety: must be called from the GTK main thread.
 */
extern void gtk_bgbox_set_background (GtkWidget *widget, int bg_type, guint32 tintcolor, gint alpha);


#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* __GTK_BGBOX_H__ */
