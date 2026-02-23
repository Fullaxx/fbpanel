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
 * gtkbar.c - GtkBar: a custom flow-layout container for fbpanel.
 *
 * Overview
 * --------
 * GtkBar is a GtkBox subclass that lays out its children in a grid of
 * fixed-size cells, flowing either left-to-right then top-to-bottom
 * (horizontal orientation) or top-to-bottom then left-to-right (vertical
 * orientation).  It is used by fbpanel to arrange plugin buttons in a
 * uniform grid on the panel bar.
 *
 * Layout model
 * ------------
 * The "dimension" field controls how many rows (horizontal mode) or columns
 * (vertical mode) the bar uses.  Child widgets are placed in cells of
 * (child_width x child_height) pixels.  Cells are separated by the GtkBox
 * spacing value.
 *
 * For horizontal orientation:
 *   rows = MIN(dimension, nvis_children)
 *   cols = ceil(nvis_children / rows)
 *   Total width  = child_width * cols + spacing * (cols - 1)
 *   Total height = child_height * rows + spacing * (rows - 1)
 *
 * For vertical orientation, rows and cols are swapped.
 *
 * Children are laid out left-to-right, row by row, starting from the
 * top-left corner.  Invisible (hidden) children are skipped in both
 * counting and placement — this allows plugins to be hidden without
 * re-ordering the grid.
 *
 * Inheritance chain
 * -----------------
 *   GObject -> GInitiallyUnowned -> GtkObject -> GtkWidget
 *           -> GtkContainer -> GtkBox -> GtkBar
 *
 * Widget lifecycle
 * ----------------
 * GtkBar is a GtkWidget subclass; it participates in the standard GTK2
 * widget lifecycle: realise -> map -> size-request -> size-allocate ->
 * expose -> unmap -> unrealise -> destroy -> finalize.
 *
 * Memory
 * ------
 * gtk_bar_new() returns a floating GtkObject reference.  Adding it to a
 * container (e.g. gtk_container_add) sinks the float and takes ownership.
 * If NOT added to a container, call g_object_ref_sink() or gtk_object_sink()
 * before eventually calling g_object_unref().
 *
 * GtkBox::children is a GList of GtkBoxChild structs; GtkBox manages this
 * list; GtkBar does not need to free it.
 */

#include "gtkbar.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * MAX_CHILD_SIZE - maximum pixel dimension (width or height) for a child cell.
 *
 * Defined but NOT used anywhere in this file.  It appears to be a leftover
 * constant from an earlier version of the layout algorithm.
 */
#define MAX_CHILD_SIZE 150

/* Forward declarations for static (file-private) functions. */
static void gtk_bar_class_init    (GtkBarClass   *klass);
static void gtk_bar_size_request  (GtkWidget *widget, GtkRequisition *requisition);
static void gtk_bar_size_allocate (GtkWidget *widget, GtkAllocation  *allocation);
//static gint gtk_bar_expose        (GtkWidget *widget, GdkEventExpose *event);

/*
 * ceilf - ceiling of a float, returning a float.
 *
 * This forward declaration is provided because on some older C89/C90
 * compilation environments <math.h> may not declare ceilf(), or the
 * code was written without including <math.h>.
 *
 * NOTE: This declaration without including <math.h> is a potential
 * portability hazard.  On C99+ compilers with <math.h>, ceilf is
 * already declared and this declaration may conflict or be redundant.
 * Including <math.h> and removing this line would be cleaner.
 */
float ceilf(float x);

/*
 * parent_class - pointer to GtkBoxClass, used in gtk_bar_class_init.
 *
 * Set once via g_type_class_peek_parent() during class initialisation.
 * This pattern allows calling parent class vfuncs if needed.
 * Currently not used for chaining in GtkBar (no super-vfunc calls),
 * but kept as standard GObject subclassing practice.
 */
static GtkBoxClass *parent_class = NULL;

/*
 * gtk_bar_get_type - GObject type registration for GtkBar.
 *
 * Returns: the GType ID for GtkBar, registering it on the first call.
 *
 * GtkBar inherits from GTK_TYPE_BOX (GtkBox), which provides the children
 * GList, spacing, and child management infrastructure.
 * Thread-safety: NOT thread-safe; acceptable for single-threaded GTK apps.
 */
GType
gtk_bar_get_type (void)
{
    static GType bar_type = 0;

    if (!bar_type)
    {
        static const GTypeInfo bar_info =
            {
                sizeof (GtkBarClass),
                NULL,		/* base_init */
                NULL,		/* base_finalize */
                (GClassInitFunc) gtk_bar_class_init,
                NULL,		/* class_finalize */
                NULL,		/* class_data */
                sizeof (GtkBar),
                0,		/* n_preallocs */
                NULL            /* instance_init: GtkBar uses no per-instance init */
            };

        // Register as a GtkBox subclass named "GtkBar"
        bar_type = g_type_register_static (GTK_TYPE_BOX, "GtkBar",
              &bar_info, 0);
    }

    return bar_type;
}

/*
 * gtk_bar_class_init - GObject class initialiser for GtkBarClass.
 *
 * Parameters:
 *   class - the GtkBarClass being initialised.
 *
 * Responsibilities:
 *   1. Saves a reference to the parent (GtkBoxClass) for potential super-calls.
 *   2. Overrides GtkWidgetClass::size_request with gtk_bar_size_request,
 *      replacing GtkBox's default horizontal/vertical packing logic.
 *   3. Overrides GtkWidgetClass::size_allocate with gtk_bar_size_allocate.
 *   4. The expose_event override (gtk_bar_expose) is commented out, so
 *      GtkBox's default expose handler is used (draws children normally).
 *
 * No finalize override is installed; GtkBar adds no heap allocations beyond
 * what GtkBox already manages, so GtkBox's finalize suffices.
 */
static void
gtk_bar_class_init (GtkBarClass *class)
{
    GtkWidgetClass *widget_class;

    parent_class = g_type_class_peek_parent (class);  // save GtkBoxClass pointer
    widget_class = (GtkWidgetClass*) class;

    widget_class->size_request  = gtk_bar_size_request;   // custom grid size request
    widget_class->size_allocate = gtk_bar_size_allocate;  // custom grid size allocate
    //widget_class->expose_event = gtk_bar_expose;        // disabled; use GtkBox default

}


/*
 * gtk_bar_new - create a new GtkBar widget.
 *
 * Parameters:
 *   orient       - GTK_ORIENTATION_HORIZONTAL: rows are primary dimension;
 *                  GTK_ORIENTATION_VERTICAL:   columns are primary dimension.
 *   spacing      - pixel gap between adjacent cells (both horizontally and
 *                  vertically).  Stored in GtkBox::spacing.
 *   child_height - height in pixels for each child cell.  Clamped to >= 1.
 *   child_width  - width in pixels for each child cell.  Clamped to >= 1.
 *
 * Returns: a new GtkBar widget with a floating reference (standard GTK2
 *          convention for widgets returned by *_new() functions).
 *          Caller should add it to a container (which sinks the float), or
 *          call g_object_ref_sink() before g_object_unref() if not parented.
 *
 * Initial state:
 *   - dimension = 1: bar starts as a single row (or column); call
 *     gtk_bar_set_dimension() to change it.
 *   - GtkBox::spacing is set directly on the GtkBox base struct.
 *
 * NOTE: Accessing GTK_BOX(bar)->spacing directly is an internal GtkBox
 * field access; this is acceptable in GTK2 but would break in GTK3+ where
 * the struct is fully opaque.
 */
GtkWidget*
gtk_bar_new(GtkOrientation orient, gint spacing,
    gint child_height, gint child_width)
{
    GtkBar *bar;

    bar = g_object_new (GTK_TYPE_BAR, NULL);  // allocate; no instance_init registered
    GTK_BOX (bar)->spacing = spacing;          // set inter-cell spacing on base GtkBox struct
    bar->orient       = orient;
    bar->child_width  = MAX(1, child_width);   // clamp to at least 1 pixel to avoid zero-size
    bar->child_height = MAX(1, child_height);  // clamp to at least 1 pixel to avoid zero-size
    bar->dimension    = 1;                     // start with 1 row (or 1 col); caller can adjust
    return (GtkWidget *)bar;
}

/*
 * gtk_bar_set_dimension - set the primary dimension (row or column count).
 *
 * Parameters:
 *   bar       - a valid GtkBar instance.
 *   dimension - the new number of rows (horizontal) or columns (vertical).
 *               Clamped to >= 1.
 *
 * If the dimension actually changes, queues a resize so the widget
 * recalculates its size request and layout.
 *
 * NOTE: dimension is clamped with MAX(1, dimension) twice in this function:
 * once on the local variable before the comparison, and once when assigning
 * to bar->dimension.  The second clamp is redundant since the variable is
 * already clamped, but it is harmless.
 */
void
gtk_bar_set_dimension(GtkBar *bar, gint dimension)
{
    dimension = MAX(1, dimension);              // clamp input to >= 1
    if (bar->dimension != dimension) {
        bar->dimension = MAX(1, dimension);     // redundant clamp; dimension already >= 1 here
        gtk_widget_queue_resize(GTK_WIDGET(bar)); // trigger size-request + size-allocate cycle
    }
}

/*
 * gtk_bar_get_dimension - return the current primary dimension.
 *
 * Parameters:
 *   bar - a valid GtkBar instance.
 *
 * Returns: the current dimension (number of rows or columns).
 *
 * Simple accessor; no side effects.
 */
gint gtk_bar_get_dimension(GtkBar *bar)
{
    return bar->dimension;
}

/*
 * gtk_bar_size_request - GtkWidget::size_request override.
 *
 * Parameters:
 *   widget      - the GtkBar widget (cast to GtkBox and GtkBar internally).
 *   requisition - output: filled with the widget's desired width and height.
 *
 * This function overrides GtkBox's size_request to implement a grid layout.
 * It iterates over all children, counts the visible ones, and computes the
 * minimum size needed to display them all in a rows x cols grid.
 *
 * Algorithm:
 *   1. Count visible children (nvis_children).
 *   2. If none, return a minimal 2x2 requisition (prevents zero-size widget).
 *   3. Compute rows and cols from dimension and nvis_children:
 *      Horizontal: rows = MIN(dimension, nvis_children),
 *                  cols = ceil(nvis_children / rows)
 *      Vertical:   cols = MIN(dimension, nvis_children),
 *                  rows = ceil(nvis_children / cols)
 *   4. Compute total width/height from child size and spacing.
 *
 * Side effect: calls gtk_widget_size_request() on every visible child.
 *   This is required so children can calculate their own preferred sizes
 *   (particularly important for GtkLabel which needs this before allocation).
 *   The child's reported size is intentionally NOT used in the grid calculation
 *   (all cells have the same fixed size from bar->child_width/height).
 *
 * NOTE: The function calls gtk_widget_size_request() on children but discards
 * the result.  This is intentional — the comment "Do not remove child request"
 * explains that GtkLabel's layout depends on size_request running before
 * size_allocate.  This is a known GTK2 quirk.
 *
 * NOTE: If dimension > nvis_children, dim is clamped to nvis_children via
 * MIN(), preventing division-by-zero in the ceil() calculation (since dim
 * would be the denominator as rows or cols and is at least 1 after the clamp).
 */
static void
gtk_bar_size_request(GtkWidget *widget, GtkRequisition *requisition)
{
    GtkBox *box = GTK_BOX(widget);   // access GtkBox base fields (children, spacing)
    GtkBar *bar = GTK_BAR(widget);;  // access GtkBar fields (child_width, child_height, etc.)
    GtkBoxChild *child;
    GList *children;
    gint nvis_children, rows, cols, dim;

    // Count visible children; also drive each child's own size_request
    nvis_children = 0;
    children = box->children;
    while (children) {
        child = children->data;
        children = children->next;

        if (GTK_WIDGET_VISIBLE(child->widget))	{
            GtkRequisition child_requisition;

            /* Do not remove child request !!! Label's proper layout depends
             * on request running before alloc. */
            gtk_widget_size_request(child->widget, &child_requisition); // result is discarded
            nvis_children++;
        }
    }
    DBG("nvis_children=%d\n", nvis_children);
    if (!nvis_children) {
        // No visible children: return a minimal non-zero size to avoid layout
        // problems with zero-size widgets in GtkBox containers.
        requisition->width = 2;
        requisition->height = 2;
        return;
    }
    // Clamp dimension to available children count (prevents dim=0 if dimension > nvis_children)
    dim = MIN(bar->dimension, nvis_children);
    if (bar->orient == GTK_ORIENTATION_HORIZONTAL) {
        rows = dim;                                             // dim controls row count
        cols = (gint) ceilf((float) nvis_children / rows);     // columns = ceil(n/rows)
    } else {
        cols = dim;                                             // dim controls column count
        rows = (gint) ceilf((float) nvis_children / cols);     // rows = ceil(n/cols)
    }

    // Total size = cell_size * count + spacing * (count - 1)
    requisition->width  = bar->child_width  * cols + box->spacing * (cols - 1);
    requisition->height = bar->child_height * rows + box->spacing * (rows - 1);
    DBG("width=%d, height=%d\n", requisition->width, requisition->height);
}

/*
 * gtk_bar_size_allocate - GtkWidget::size_allocate override.
 *
 * Parameters:
 *   widget     - the GtkBar widget.
 *   allocation - the rectangle (x, y, width, height) assigned by the parent
 *                container; this is the space GtkBar must fit into.
 *
 * Called by GTK after size_request, with the actual space the parent has
 * decided to give us (may differ from our request).  Must place all visible
 * children within this space.
 *
 * Algorithm:
 *   1. Count visible children.
 *   2. Compute rows/cols as in size_request.
 *   3. Compute child cell size:
 *      child_width  = MIN(available_width  / cols, bar->child_width)
 *      child_height = MIN(available_height / rows, bar->child_height)
 *      (clamped to >= 1)
 *   4. Walk visible children, placing them left-to-right, top-to-bottom,
 *      wrapping to a new row after every `cols` children.
 *
 * The allocation is stored in widget->allocation (GtkWidget base struct)
 * so that the widget knows its current screen position.
 *
 * gtk_widget_queue_draw() is called after computing layout but before
 * allocating children — this forces a repaint of the bar background even
 * if child positions have not changed.
 *
 * NOTE: Children that exceed bar->child_width / bar->child_height (e.g. if
 * the container gives more space than requested) are capped to the configured
 * cell size via MIN().  This intentionally prevents children from growing
 * larger than the configured cell size even if extra space is available.
 *
 * NOTE: If allocation->width is very small (cols * spacing >= allocation->width),
 * tmp / cols could be 0 or negative before the clamp to >= 1.  The clamp
 * ensures no zero-size allocation is passed to children, preventing X errors,
 * but the layout will overlap or be clipped.
 *
 * NOTE: Invisible children are skipped in placement but they DO occupy slots
 * in the GList iteration; the `tmp` column counter only advances for visible
 * children, so row-wrapping is based on visible children count only.  This
 * means hidden children do not leave gaps in the grid — the next visible child
 * fills in where the hidden one would have been.
 */
static void
gtk_bar_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
    GtkBox *box;
    GtkBar *bar;
    GtkBoxChild *child;
    GList *children;
    GtkAllocation child_allocation;  // scratch: current cell position/size for each child
    gint nvis_children, tmp, rows, cols, dim;

    ENTER;
    DBG("a.w=%d  a.h=%d\n", allocation->width, allocation->height);
    box = GTK_BOX (widget);
    bar = GTK_BAR (widget);
    widget->allocation = *allocation;  // store our actual allocated rectangle

    // Count visible children first (needed to compute grid dimensions)
    nvis_children = 0;
    children = box->children;
    while (children) {
        child = children->data;
        children = children->next;

        if (GTK_WIDGET_VISIBLE (child->widget))
            nvis_children += 1;
    }
    gtk_widget_queue_draw(widget);   // force redraw even if child allocations are unchanged

    dim = MIN(bar->dimension, nvis_children);  // clamp dimension to actual child count
    if (nvis_children == 0)
        RET();  // nothing to allocate; return early

    // Compute grid dimensions using same formula as size_request
    if (bar->orient == GTK_ORIENTATION_HORIZONTAL) {
        rows = dim;                                          // horizontal: dim = rows
        cols = (gint) ceilf((float) nvis_children / rows);  // cols = ceil(n/rows)
    } else {
        cols = dim;                                          // vertical: dim = cols
        rows = (gint) ceilf((float) nvis_children / cols);  // rows = ceil(n/cols)
    }
    DBG("rows=%d cols=%d\n", rows, cols);

    // Compute child cell width: divide available width among cols, capped at child_width
    tmp = allocation->width - (cols - 1) * box->spacing;   // total width minus inter-cell gaps
    child_allocation.width = MIN(tmp / cols, bar->child_width);  // per-cell width, capped

    // Compute child cell height: divide available height among rows, capped at child_height
    tmp = allocation->height - (rows - 1) * box->spacing;  // total height minus inter-cell gaps
    child_allocation.height = MIN(tmp / rows, bar->child_height); // per-cell height, capped

    // Clamp to at least 1 pixel to prevent BadValue X errors on zero-size windows
    if (child_allocation.width < 1)
        child_allocation.width = 1;
    if (child_allocation.height < 1)
        child_allocation.height = 1;
    DBG("child alloc: width=%d height=%d\n",
        child_allocation.width,
        child_allocation.height);

    // Start placing children at the top-left of our allocated rectangle
    child_allocation.x = allocation->x;
    child_allocation.y = allocation->y;
    children = box->children;  // restart list iteration from the beginning
    tmp = 0;                   // column counter: tracks position within current row
    while (children) {
        child = children->data;
        children = children->next;

        if (GTK_WIDGET_VISIBLE (child->widget)) {
            DBG("allocate x=%d y=%d\n", child_allocation.x,
                child_allocation.y);
            gtk_widget_size_allocate(child->widget, &child_allocation);  // place child in cell
            tmp++;  // advance column counter
            if (tmp == cols) {
                // End of row: wrap to the next row
                child_allocation.x  = allocation->x;                        // reset to left edge
                child_allocation.y += child_allocation.height + box->spacing; // move down one row
                tmp = 0;  // reset column counter for new row
            } else {
                // Same row: advance to the next column
                child_allocation.x += child_allocation.width + box->spacing;
            }
        }
    }
    RET();
}


/*
 * gtk_bar_expose - DISABLED custom expose handler (compiled out with #if 0).
 *
 * This block is conditionally excluded from compilation.  It was apparently
 * an attempt to paint the bar background before the children expose, using
 * gtk_paint_flat_box().  It has at least one bug: `w` and `h` are declared
 * but never assigned (they would contain garbage values when passed to
 * gtk_paint_flat_box), and the DBG call would print undefined values.
 *
 * The commented-out expose override in gtk_bar_class_init means GTK2's
 * default container expose handler is used instead, which simply chains
 * to each child's own expose handler.
 *
 * If this code were re-enabled as-is, it would likely produce visual
 * corruption or a crash due to the uninitialised w and h variables.
 */
#if 0
static gint
gtk_bar_expose (GtkWidget *widget, GdkEventExpose *event)
{
    ENTER;

    if (GTK_WIDGET_DRAWABLE (widget)) {
        int w, h;  // BUG: w and h are declared but never initialised here!

        DBG("w, h = %d,%d\n", w, h);  // BUG: prints garbage values
        if (!GTK_WIDGET_APP_PAINTABLE (widget))
            gtk_paint_flat_box (widget->style, widget->window,
                  widget->state, GTK_SHADOW_NONE,
                  NULL /*&event->area*/, widget, NULL,
                  0, 0, w, h);  // BUG: w, h are uninitialised; undefined behaviour
    }
    RET(FALSE);
}
#endif
