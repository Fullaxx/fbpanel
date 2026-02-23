/*
 * gconf.h -- GTK preferences dialog helper types and widget constructors.
 *
 * Provides the gconf_block structure and widget factory functions used
 * by gconf_panel.c and gconf_plugins.c to build the "Configure Panel"
 * preferences dialog.
 *
 * A gconf_block groups a set of related configuration widgets into a
 * labeled section.  Each widget is backed by an xconf node; changes made
 * in the UI are written back to the xconf tree, then the panel reloads
 * its configuration.
 *
 * Usage pattern:
 *   gconf_block *b = gconf_block_new(my_callback, my_data, indent);
 *   gconf_edit_int(b, xc_width, 1, 9999);
 *   gconf_edit_enum(b, xc_edge, edge_enum);
 *   gconf_edit_boolean(b, xc_autohide, "Autohide");
 *   // pack b->main into a dialog page
 *   // later:
 *   gconf_block_free(b);
 */

#ifndef _GCONF_H_
#define _GCONF_H_

#include <gtk/gtk.h>
#include "panel.h"   /* xconf, xconf_enum types */

/*
 * gconf_block -- a section of configuration UI widgets.
 *
 * Fields:
 *   main  - top-level container widget for this block (a GtkVBox or similar).
 *           Pack this into a dialog page or notebook tab.
 *   area  - inner container where row widgets are packed.
 *   cb    - callback invoked when any widget in this block changes value.
 *           Signature: void cb(gpointer data).
 *   data  - user data passed to cb on each change.
 *   rows  - GSList of GtkWidget* row containers (one per added widget row).
 *           Used internally by gconf_block_add(); do not modify directly.
 *   sgr   - GtkSizeGroup for aligning labels across rows within this block.
 *           Ensures all label widgets have the same width for visual alignment.
 */
typedef struct
{
    GtkWidget *main, *area;   /* outer and inner containers */
    GCallback cb;             /* change notification callback */
    gpointer data;            /* user data for cb */
    GSList *rows;             /* list of row container widgets */
    GtkSizeGroup *sgr;        /* size group for label alignment */
} gconf_block;


/*
 * gconf_block_new -- allocate and initialise a new gconf_block.
 *
 * Parameters:
 *   cb     - callback to invoke when a widget value changes.
 *   data   - passed to cb as user_data.
 *   indent - left-indent in pixels applied to the area container.
 *
 * Returns: newly allocated gconf_block*.  Caller owns it; free with
 *          gconf_block_free() when the dialog is destroyed.
 *
 * Memory: allocates the block struct and creates GTK widgets.
 *         Widget refs are owned by the GTK container hierarchy; the block
 *         struct itself is g_malloc'd and must be g_free'd via gconf_block_free.
 */
gconf_block *gconf_block_new(GCallback cb, gpointer data, int indent);

/*
 * gconf_block_free -- free a gconf_block and release its GSList.
 *
 * Parameters:
 *   b - the block to free.  The GTK widgets inside are owned by their
 *       parent containers and are NOT destroyed here; they are cleaned
 *       up when the dialog's top-level widget is destroyed.
 *
 * Memory: frees b->rows GSList and the block struct itself (g_free).
 */
void gconf_block_free(gconf_block *b);

/*
 * gconf_block_add -- add a widget to a gconf_block.
 *
 * Parameters:
 *   b       - the block to add to.
 *   w       - the widget to add (e.g., a spin button, combo box).
 *   new_row - if TRUE, start a new row; if FALSE, append to the current row.
 *
 * Used internally by gconf_edit_*; not normally called by external code.
 */
void gconf_block_add(gconf_block *b, GtkWidget *w, gboolean new_row);

/*
 * gconf_edit_int -- add an integer spin-button editor to a block.
 *
 * Parameters:
 *   b   - the block to add to.
 *   xc  - the xconf node whose value is being edited.
 *         The current value is read with xconf_get_int(); the widget
 *         writes back with xconf_set_int() on change, then calls b->cb.
 *   min - minimum allowed value (inclusive).
 *   max - maximum allowed value (inclusive).
 *
 * Returns: the GtkSpinButton widget added (owned by the block's container).
 */
GtkWidget *gconf_edit_int(gconf_block *b, xconf *xc, int min, int max);

/*
 * gconf_edit_enum -- add an enum combo-box editor to a block.
 *
 * Parameters:
 *   b  - the block to add to.
 *   xc - the xconf node being edited; current value matched against 'e'.
 *   e  - NULL-terminated xconf_enum array mapping strings to int values.
 *        The combo shows e[i].desc strings; selection writes e[i].num to xc.
 *
 * Returns: the GtkComboBox widget added.
 */
GtkWidget *gconf_edit_enum(gconf_block *b, xconf *xc, xconf_enum *e);

/*
 * gconf_edit_boolean -- add a labelled checkbox editor to a block.
 *
 * Parameters:
 *   b    - the block to add to.
 *   xc   - the xconf node being edited ("0"/"1" or "false"/"true").
 *   text - label shown next to the checkbox.
 *
 * Returns: the GtkCheckButton widget added.
 */
GtkWidget *gconf_edit_boolean(gconf_block *b, xconf *xc, gchar *text);

/*
 * gconf_edit_color -- add a colour + alpha editor to a block.
 *
 * Parameters:
 *   b        - the block to add to.
 *   xc_color - xconf node for the colour value (hex string, e.g. "#RRGGBB").
 *   xc_alpha - xconf node for the alpha value (integer 0–255).
 *
 * Returns: the GtkColorButton widget added.
 */
GtkWidget *gconf_edit_color(gconf_block *b, xconf *xc_color, xconf *xc_alpha);

#endif
