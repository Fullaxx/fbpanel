/**
 * @file
 * @brief GTK preferences dialog helper types and widget constructors.
 *
 * Provides the ::gconf_block structure and widget factory functions used
 * by gconf_panel.c and gconf_plugins.c to build the "Configure Panel"
 * preferences dialog.
 *
 * A gconf_block groups a set of related configuration widgets into a
 * labeled section. Each widget is backed by an xconf node; changes made
 * in the UI are written back to the xconf tree, then the panel reloads
 * its configuration.
 *
 * @par Usage pattern
 * @code
 *   gconf_block *b = gconf_block_new(my_callback, my_data, indent);
 *   gconf_edit_int(b, xc_width, 1, 9999);
 *   gconf_edit_enum(b, xc_edge, edge_enum);
 *   gconf_edit_boolean(b, xc_autohide, "Autohide");
 *   // pack b->main into a dialog page
 *   // later:
 *   gconf_block_free(b);
 * @endcode
 */

#ifndef _GCONF_H_
#define _GCONF_H_

#include <gtk/gtk.h>
#include "panel.h"   /* xconf, xconf_enum types */

/**
 * @brief A section of configuration UI widgets.
 */
typedef struct
{
    GtkWidget *main;    /**< Top-level container widget for this block (a GtkVBox or similar); pack this into a dialog page or notebook tab. */
    GtkWidget *area;    /**< Inner container where row widgets are packed. */
    GCallback cb;       /**< Callback invoked when any widget in this block changes value; signature `void cb(gpointer data)`. */
    gpointer data;      /**< User data passed to #cb on each change. */
    GSList *rows;       /**< GSList of GtkWidget* row containers (one per added widget row). Used internally by gconf_block_add(); do not modify directly. */
    GtkSizeGroup *sgr;  /**< GtkSizeGroup for aligning labels across rows within this block, so all label widgets share the same width. */
} gconf_block;


/**
 * @brief Allocate and initialise a new gconf_block.
 *
 * Allocates the block struct and creates GTK widgets. Widget refs are
 * owned by the GTK container hierarchy; the block struct itself is
 * g_malloc'd and must be g_free'd via gconf_block_free().
 *
 * @param cb     Callback to invoke when a widget value changes.
 * @param data   Passed to @p cb as user_data.
 * @param indent Left-indent in pixels applied to the area container.
 * @return Newly allocated gconf_block*. Caller owns it; free with
 *         gconf_block_free() when the dialog is destroyed.
 */
gconf_block *gconf_block_new(GCallback cb, gpointer data, int indent);

/**
 * @brief Free a gconf_block and release its GSList.
 *
 * Frees `b->rows` and the block struct itself (g_free()).
 *
 * @param b The block to free. The GTK widgets inside are owned by their
 *          parent containers and are NOT destroyed here; they are cleaned
 *          up when the dialog's top-level widget is destroyed.
 */
void gconf_block_free(gconf_block *b);

/**
 * @brief Add a widget to a gconf_block.
 *
 * Used internally by `gconf_edit_*`; not normally called by external code.
 *
 * @param b       The block to add to.
 * @param w       The widget to add (e.g., a spin button, combo box).
 * @param new_row If TRUE, start a new row; if FALSE, append to the current row.
 */
void gconf_block_add(gconf_block *b, GtkWidget *w, gboolean new_row);

/**
 * @brief Add an integer spin-button editor to a block.
 *
 * The current value is read with xconf_get_int(); the widget writes back
 * with xconf_set_int() on change, then calls `b->cb`.
 *
 * @param b   The block to add to.
 * @param xc  The xconf node whose value is being edited.
 * @param min Minimum allowed value (inclusive).
 * @param max Maximum allowed value (inclusive).
 * @return The GtkSpinButton widget added (owned by the block's container).
 */
GtkWidget *gconf_edit_int(gconf_block *b, xconf *xc, int min, int max);

/**
 * @brief Add an enum combo-box editor to a block.
 *
 * The combo shows `e[i].desc` strings; selection writes `e[i].num` to @p xc.
 *
 * @param b  The block to add to.
 * @param xc The xconf node being edited; current value matched against @p e.
 * @param e  NULL-terminated xconf_enum array mapping strings to int values.
 * @return The GtkComboBox widget added.
 */
GtkWidget *gconf_edit_enum(gconf_block *b, xconf *xc, xconf_enum *e);

/**
 * @brief Add a labelled checkbox editor to a block.
 *
 * @param b    The block to add to.
 * @param xc   The xconf node being edited ("0"/"1" or "false"/"true").
 * @param text Label shown next to the checkbox.
 * @return The GtkCheckButton widget added.
 */
GtkWidget *gconf_edit_boolean(gconf_block *b, xconf *xc, gchar *text);

/**
 * @brief Add a colour + alpha editor to a block.
 *
 * @param b        The block to add to.
 * @param xc_color xconf node for the colour value (hex string, e.g. "#RRGGBB").
 * @param xc_alpha xconf node for the alpha value (integer 0-255).
 * @return The GtkColorButton widget added.
 */
GtkWidget *gconf_edit_color(gconf_block *b, xconf *xc_color, xconf *xc_alpha);

#endif
