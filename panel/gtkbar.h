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

/**
 * @file
 * @brief Public interface for GtkBar, a flow-wrapping layout container.
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
 * @verbatim
   GObject
   +-- GtkObject
       +-- GtkWidget
           +-- GtkContainer
               +-- GtkBox
                   +-- GtkBar   <- this widget
   @endverbatim
 *
 * @note GtkBar instances are added to the panel's GtkBox (or a plugin's
 *       pwid) via gtk_container_add(). The parent container owns the
 *       reference. Callers must not gtk_widget_destroy() a GtkBar that is
 *       still a child of a container; destroying the parent propagates to
 *       children automatically.
 * @note Vendored from GTK+ (Peter Mattis, Spencer Kimball, Josh
 *       MacDonald; 1995-1997), licensed LGPL-2.0-or-later, and adapted
 *       into GtkBar. See docs/THIRD_PARTY_NOTICES.md.
 */

#ifndef __GTK_BAR_H__
#define __GTK_BAR_H__


#include <gdk/gdk.h>
#include <gtk/gtkbox.h>   /* GtkBox base class */


#ifdef __cplusplus
//extern "C" {
#endif /* __cplusplus */


/**
 * @name GObject type macros for GtkBar.
 * @{
 * @def GTK_TYPE_BAR
 *   GType constant for GtkBar; used in g_object_new() etc.
 * @def GTK_BAR(obj)
 *   Safe downcast from GObject* to GtkBar* (type-checked in debug).
 * @def GTK_BAR_CLASS(klass)
 *   Safe downcast of a GObjectClass* to GtkBarClass*.
 * @def GTK_IS_BAR(obj)
 *   Runtime type predicate.
 * @def GTK_IS_BAR_CLASS(klass)
 *   Runtime class type predicate.
 * @def GTK_BAR_GET_CLASS(obj)
 *   Retrieve the GtkBarClass* vtable from an instance.
 * @}
 */
#define GTK_TYPE_BAR            (gtk_bar_get_type ())
#define GTK_BAR(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_BAR, GtkBar))
#define GTK_BAR_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_BAR, GtkBarClass))
#define GTK_IS_BAR(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_BAR))
#define GTK_IS_BAR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_BAR))
#define GTK_BAR_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_BAR, GtkBarClass))


typedef struct _GtkBar       GtkBar;
typedef struct _GtkBarClass  GtkBarClass;

/**
 * @brief Instance data for one GtkBar widget.
 */
struct _GtkBar
{
    GtkBox box;              /**< Parent instance; must be first for safe casting to/from GtkBox*. */
    gint child_height;       /**< Fixed height allocated to each child widget, in pixels. */
    gint child_width;        /**< Fixed width allocated to each child widget, in pixels. */
    gint dimension;          /**< Max children per row (horizontal bar) or column (vertical bar) before wrapping. Updated via gtk_bar_set_dimension(). */
    GtkOrientation orient;   /**< GTK_ORIENTATION_HORIZONTAL or GTK_ORIENTATION_VERTICAL; set at construction, used by size_allocate to pick the tiling direction. */
};

/**
 * @brief Class (vtable) for GtkBar.
 *
 * No additional virtual functions beyond GtkBox's class. `size_request`
 * and `size_allocate` are overridden in gtkbar.c.
 */
struct _GtkBarClass
{
    GtkBoxClass parent_class;  /**< Must be first; GtkBox vtable is inherited. */
};


/**
 * @brief Return the GType for GtkBar.
 *
 * Registers the type on first call. Used by the #GTK_TYPE_BAR and
 * #GTK_BAR() macros; normally not called directly.
 *
 * @return The GType ID for GtkBar.
 */
GType	   gtk_bar_get_type (void) G_GNUC_CONST;

/**
 * @brief Create a new GtkBar widget.
 *
 * @param orient       GTK_ORIENTATION_HORIZONTAL tiles children
 *                     left-to-right, wrapping to a new row after
 *                     @p dimension children set via
 *                     gtk_bar_set_dimension(). GTK_ORIENTATION_VERTICAL
 *                     tiles top-to-bottom, wrapping to a new column.
 * @param spacing      Pixel gap between children (passed to the
 *                     underlying GtkBox).
 * @param child_height Fixed height in pixels for each child allocation slot.
 * @param child_width  Fixed width in pixels for each child allocation slot.
 * @return A new GtkBar widget with a floating reference. The caller must
 *         add it to a container (which sinks the ref) or call
 *         g_object_ref_sink() explicitly.
 * @note The widget is freed when all references are dropped, typically
 *       when the parent container is destroyed.
 */
GtkWidget* gtk_bar_new(GtkOrientation orient,
    gint spacing, gint child_height, gint child_width);

/**
 * @brief Update the tiling threshold.
 *
 * Queues a resize so the new layout takes effect on the next GTK layout
 * pass. Used by taskbar when the number of visible task rows changes.
 *
 * @param bar       The GtkBar to update.
 * @param dimension New max children per row/column (>= 1). Setting to 0
 *                  or negative is undefined behaviour.
 */
void gtk_bar_set_dimension(GtkBar *bar, gint dimension);

/**
 * @brief Read the current tiling threshold.
 *
 * @param bar The GtkBar to query.
 * @return The current value of `bar->dimension`.
 */
gint gtk_bar_get_dimension(GtkBar *bar);

#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* __GTK_BAR_H__ */
