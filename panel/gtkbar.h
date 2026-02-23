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
 * gtkbar.h -- Public interface for GtkBar, a flow-wrapping layout container.
 *
 * GtkBar is a GtkBox subclass that arranges its children in a grid-like
 * layout, tiling them into rows and columns up to a configurable maximum
 * "dimension" (number of columns for horizontal bars, rows for vertical).
 *
 * Primary use cases in fbpanel:
 *   - taskbar: tiles task buttons into rows; wraps when the panel height
 *     changes (e.g., from a two-row taskbar configuration).
 *   - launchbar: tiles launcher icon buttons in a single row/column.
 *
 * Widget hierarchy:
 *   GObject
 *   └── GtkObject
 *       └── GtkWidget
 *           └── GtkContainer
 *               └── GtkBox
 *                   └── GtkBar   ← this widget
 *
 * Ownership: GtkBar instances are added to the panel's GtkBox (or a plugin's
 * pwid) via gtk_container_add(). The parent container owns the reference.
 * Callers must not gtk_widget_destroy() a GtkBar that is still a child of
 * a container; destroying the parent propagates to children automatically.
 */

#ifndef __GTK_BAR_H__
#define __GTK_BAR_H__


#include <gdk/gdk.h>
#include <gtk/gtkbox.h>   /* GtkBox base class */


#ifdef __cplusplus
//extern "C" {
#endif /* __cplusplus */


/* --- GObject type macros --- */

/* GTK_TYPE_BAR: GType constant for GtkBar; used in g_object_new() etc. */
#define GTK_TYPE_BAR            (gtk_bar_get_type ())
/* GTK_BAR(obj): safe downcast from GObject* to GtkBar* (type-checked in debug) */
#define GTK_BAR(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_BAR, GtkBar))
/* GTK_BAR_CLASS(klass): safe downcast of a GObjectClass* to GtkBarClass* */
#define GTK_BAR_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_BAR, GtkBarClass))
/* GTK_IS_BAR(obj): runtime type predicate */
#define GTK_IS_BAR(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_BAR))
/* GTK_IS_BAR_CLASS(klass): runtime class type predicate */
#define GTK_IS_BAR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_BAR))
/* GTK_BAR_GET_CLASS(obj): retrieve the GtkBarClass* vtable from an instance */
#define GTK_BAR_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_BAR, GtkBarClass))


typedef struct _GtkBar       GtkBar;
typedef struct _GtkBarClass  GtkBarClass;

/*
 * struct _GtkBar -- instance data for one GtkBar widget.
 *
 * Fields (in addition to inherited GtkBox fields):
 *   box          - embedded GtkBox (MUST be first; allows casting GtkBar* to GtkBox*)
 *   child_height - fixed height allocated to each child widget (pixels)
 *   child_width  - fixed width allocated to each child widget (pixels)
 *   dimension    - maximum number of children per row (horizontal bar) or
 *                  column (vertical bar) before wrapping to the next row/col.
 *                  Updated via gtk_bar_set_dimension().
 *   orient       - GTK_ORIENTATION_HORIZONTAL or GTK_ORIENTATION_VERTICAL;
 *                  set at construction time and used by size_allocate to
 *                  determine tiling direction.
 */
struct _GtkBar
{
    GtkBox box;              /* parent instance; must be first for safe casting */
    gint child_height;       /* height per child slot in pixels */
    gint child_width;        /* width per child slot in pixels */
    gint dimension;          /* max children per row/column (wrapping threshold) */
    GtkOrientation orient;   /* layout direction: horizontal or vertical */
};

/*
 * struct _GtkBarClass -- class (vtable) for GtkBar.
 *
 * No additional virtual functions beyond GtkBox's class.
 * size_request and size_allocate are overridden in gtkbar.c.
 */
struct _GtkBarClass
{
    GtkBoxClass parent_class;  /* must be first; GtkBox vtable is inherited */
};


/*
 * gtk_bar_get_type -- return the GType for GtkBar.
 *
 * Registers the type on first call.  Used by the GTK_TYPE_BAR and GTK_BAR()
 * macros; normally not called directly.
 *
 * Returns: the GType ID for GtkBar.
 */
GType	   gtk_bar_get_type (void) G_GNUC_CONST;

/*
 * gtk_bar_new -- create a new GtkBar widget.
 *
 * Parameters:
 *   orient       - GTK_ORIENTATION_HORIZONTAL: children are tiled left-to-right,
 *                  wrapping to a new row after 'dimension' children.
 *                  GTK_ORIENTATION_VERTICAL: children tiled top-to-bottom,
 *                  wrapping to a new column.
 *   spacing      - pixel gap between children (passed to underlying GtkBox).
 *   child_height - fixed height in pixels for each child allocation slot.
 *   child_width  - fixed width in pixels for each child allocation slot.
 *
 * Returns: a new GtkBar widget with a floating reference.
 *          The caller must add it to a container (which sinks the ref) or
 *          call g_object_ref_sink() explicitly.
 *
 * Memory: the widget is freed when all references are dropped, typically when
 * the parent container is destroyed.
 */
GtkWidget* gtk_bar_new(GtkOrientation orient,
    gint spacing, gint child_height, gint child_width);

/*
 * gtk_bar_set_dimension -- update the tiling threshold.
 *
 * Parameters:
 *   bar       - the GtkBar to update.
 *   dimension - new max children per row/column (≥ 1).
 *               Setting to 0 or negative is undefined behaviour.
 *
 * Queues a resize so the new layout takes effect on the next GTK layout pass.
 * Used by taskbar when the number of visible task rows changes.
 */
void gtk_bar_set_dimension(GtkBar *bar, gint dimension);

/*
 * gtk_bar_get_dimension -- read the current tiling threshold.
 *
 * Parameters:
 *   bar - the GtkBar to query.
 *
 * Returns: the current value of bar->dimension.
 */
gint gtk_bar_get_dimension(GtkBar *bar);

#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* __GTK_BAR_H__ */
