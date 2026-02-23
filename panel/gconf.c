/*
 * gconf.c -- fbpanel preferences dialog widget factory helpers.
 *
 * Provides gconf_block, a reusable layout container for the preferences
 * dialog, plus factory functions for creating editor widgets tied to xconf
 * config tree nodes.
 *
 * gconf_block is a horizontal box with:
 *   - An optional left indent spacer.
 *   - A vertical "area" box into which rows of labeled editors are packed.
 *   - A GtkSizeGroup to align the first label in each row horizontally.
 *
 * Editor widgets (spin buttons, combo boxes, check buttons, color buttons)
 * are connected directly to xconf nodes via GObject signals: when the user
 * changes a value in the UI, the corresponding xconf_set_* function is
 * called immediately to update the in-memory config.
 *
 * If a gconf_block has a callback (b->cb / b->data), each editor widget
 * also connects a secondary "changed" signal back to that callback so the
 * parent can react to sub-widget changes (e.g., enabling/disabling
 * dependent controls).
 *
 * Public API:
 *   gconf_block_new()     — allocate a new gconf_block.
 *   gconf_block_free()    — free a gconf_block (does NOT destroy b->main).
 *   gconf_block_add()     — add a widget to the block (new or current row).
 *   gconf_edit_int()      — create a GtkSpinButton bound to an int xconf node.
 *   gconf_edit_enum()     — create a GtkComboBox bound to an enum xconf node.
 *   gconf_edit_boolean()  — create a GtkCheckButton bound to a bool xconf node.
 *   gconf_edit_color()    — create a GtkColorButton bound to color+alpha nodes.
 */

#include "gconf.h"
#include "misc.h"

//#define DEBUGPRN
#include "dbg.h"

/* Visual indent in pixels for indented gconf_blocks */
#define INDENT_SIZE 20


/*
 * gconf_block_new -- allocate and initialise a new gconf_block.
 *
 * Creates the main GtkHBox with an optional indent spacer and a vertical
 * "area" box for rows.  A GtkSizeGroup is created to align label widgets.
 *
 * Parameters:
 *   cb     - callback fired when any child editor widget changes value
 *            (may be NULL; signature: void cb(gconf_block *b)).
 *   data   - user data passed as the first argument to cb (typically an xconf*).
 *   indent - indent in pixels; 0 = no indent, >0 = left spacer of that width.
 *
 * Returns: newly allocated gconf_block (caller owns it; free with gconf_block_free).
 *
 * Memory:
 *   b->sgr  (GtkSizeGroup) — owned by b; g_object_unref'd in gconf_block_free.
 *   b->rows (GSList)       — owned by b; g_slist_free'd in gconf_block_free.
 *   b->main (GtkWidget)    — NOT freed in gconf_block_free; caller is responsible
 *                            for destroying it (or letting the container do it).
 */
gconf_block *
gconf_block_new(GCallback cb, gpointer data, int indent)
{
    GtkWidget *w;
    gconf_block *b;

    b = g_new0(gconf_block, 1);
    b->cb = cb;
    b->data = data;
    b->main = gtk_hbox_new(FALSE, 0);   /* outer horizontal container */

    /* optional left indent spacer */
    if (indent > 0)
    {
        w = gtk_hbox_new(FALSE, 0);
        gtk_widget_set_size_request(w, indent, -1);   /* fixed width, natural height */
        gtk_box_pack_start(GTK_BOX(b->main), w, FALSE, FALSE, 0);
    }

    /* vertical area for editor rows */
    w = gtk_vbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(b->main), w, FALSE, FALSE, 0);
    b->area = w;

    /* size group aligns the first widget (label) in each row */
    b->sgr = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    return b;
}

/*
 * gconf_block_free -- release a gconf_block's resources.
 *
 * Frees the size group, the rows GSList (not the widgets), and the struct.
 * Does NOT destroy b->main or any child widgets — those are owned by their
 * GTK container and destroyed when the container is destroyed.
 *
 * Parameters:
 *   b - the gconf_block to free.
 */
void
gconf_block_free(gconf_block *b)
{
    g_object_unref(b->sgr);    /* release GtkSizeGroup reference */
    g_slist_free(b->rows);     /* free the list (not the widgets themselves) */
    g_free(b);
}

/*
 * gconf_block_add -- add a widget to a gconf_block.
 *
 * If @new_row is TRUE (or no rows exist yet), creates a new horizontal row
 * in b->area and prepends it to b->rows.  The first widget added to a new
 * row is added to the size group (for alignment) if it is a GtkMisc.
 *
 * The widget is then packed into the current row.
 *
 * Parameters:
 *   b       - the gconf_block to add to.
 *   w       - the widget to add.
 *   new_row - TRUE to start a new row, FALSE to append to the current row.
 *
 * Note: b->rows is a prepend list, so b->rows->data is always the most
 *   recently started row.
 */
void
gconf_block_add(gconf_block *b, GtkWidget *w, gboolean new_row)
{
    GtkWidget *hbox;

    if (!b->rows || new_row)
    {
        GtkWidget *s;

        new_row = TRUE;
        hbox = gtk_hbox_new(FALSE, 8);             /* new editor row */
        b->rows = g_slist_prepend(b->rows, hbox);  /* prepend; head = current row */
        gtk_box_pack_start(GTK_BOX(b->area), hbox, FALSE, FALSE, 0);
        /* trailing flexible spacer to left-align the row contents */
        s = gtk_vbox_new(FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), s, TRUE, TRUE, 0);

        /* add the first widget in a new row to the size group for alignment */
        if (GTK_IS_MISC(w))
        {
            DBG("misc \n");
            gtk_misc_set_alignment(GTK_MISC(w), 0, 0.5);
            gtk_size_group_add_widget(b->sgr, w);
        }
    }
    else
        hbox = b->rows->data;   /* reuse the current (most recent) row */
    gtk_box_pack_start(GTK_BOX(hbox), w, FALSE, FALSE, 0);
}

/*********************************************************
 * Edit int
 *********************************************************/

/*
 * gconf_edit_int_cb -- "value-changed" callback for an int spin button.
 *
 * Called when the user changes the spin button value.
 * Writes the new integer to the associated xconf node.
 */
static void
gconf_edit_int_cb(GtkSpinButton *w, xconf *xc)
{
    int i;

    i = (int) gtk_spin_button_get_value(w);
    xconf_set_int(xc, i);   /* update config tree immediately */
}

/*
 * gconf_edit_int -- create a GtkSpinButton bound to an int xconf node.
 *
 * Reads the current integer value from @xc, writes it back (to normalise),
 * then creates a spin button in the range [min, max] with step 1.
 * On change, the spin button calls gconf_edit_int_cb to update @xc.
 *
 * Parameters:
 *   b   - gconf_block to which the widget belongs (for notification; may be NULL).
 *   xc  - the xconf node to read/write the integer value.
 *   min - minimum allowed value.
 *   max - maximum allowed value.
 *
 * Returns: the new GtkSpinButton widget (not yet added to any container).
 */
GtkWidget *
gconf_edit_int(gconf_block *b, xconf *xc, int min, int max)
{
    gint i = 0;
    GtkWidget *w;

    xconf_get_int(xc, &i);       /* read current value */
    xconf_set_int(xc, i);        /* normalise (write string form back) */
    w = gtk_spin_button_new_with_range((gdouble) min, (gdouble) max, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), (gdouble) i);
    g_signal_connect(G_OBJECT(w), "value-changed",
        G_CALLBACK(gconf_edit_int_cb), xc);
    if (b && b->cb)
    {
        /* notify the parent block's callback when the value changes */
        g_signal_connect_swapped(G_OBJECT(w), "value-changed",
            G_CALLBACK(b->cb), b);
    }
    return w;
}

/*********************************************************
 * Edit enum
 *********************************************************/

/*
 * gconf_edit_enum_cb -- "changed" callback for an enum combo box.
 *
 * Called when the user selects a different item in the combo box.
 * Gets the active index, looks up the enum table stored on the widget,
 * and writes the string form of the enum value to the xconf node.
 */
static void
gconf_edit_enum_cb(GtkComboBox *w, xconf *xc)
{
    int i;

    i = gtk_combo_box_get_active(w);
    DBG("%s=%d\n", xc->name, i);
    /* the enum table was stashed on the widget in gconf_edit_enum */
    xconf_set_enum(xc, i, g_object_get_data(G_OBJECT(w), "enum"));
}

/*
 * gconf_edit_enum -- create a GtkComboBox bound to an enum xconf node.
 *
 * Reads the current enum value from @xc, normalises it, then builds a
 * combo box with one entry per xconf_enum table entry.  On change, calls
 * gconf_edit_enum_cb.
 *
 * Parameters:
 *   b  - gconf_block to notify on change (may be NULL).
 *   xc - the xconf node to read/write the enum value.
 *   e  - NULL-terminated xconf_enum table (num + str pairs).
 *
 * Returns: the new GtkComboBox widget.
 *
 * Note: The enum table pointer @e is stored on the widget via
 *   g_object_set_data(widget, "enum", e) for retrieval in the callback.
 *   @e must remain valid for the lifetime of the widget.
 */
GtkWidget *
gconf_edit_enum(gconf_block *b, xconf *xc, xconf_enum *e)
{
    gint i = 0;
    GtkWidget *w;

    xconf_get_enum(xc, &i, e);    /* read current enum value */
    xconf_set_enum(xc, i, e);     /* normalise (write string form back) */
    w = gtk_combo_box_new_text();
    g_object_set_data(G_OBJECT(w), "enum", e);  /* stash table for callback */
    while (e && e->str)
    {
        /* insert at position e->num, using translated description if available */
        gtk_combo_box_insert_text(GTK_COMBO_BOX(w), e->num,
            e->desc ? _(e->desc) : _(e->str));
        e++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(w), i);
    g_signal_connect(G_OBJECT(w), "changed",
        G_CALLBACK(gconf_edit_enum_cb), xc);
    if (b && b->cb)
    {
        g_signal_connect_swapped(G_OBJECT(w), "changed",
            G_CALLBACK(b->cb), b);
    }

    return w;
}

/*********************************************************
 * Edit boolean
 *********************************************************/

/*
 * gconf_edit_bool_cb -- "toggled" callback for a boolean check button.
 *
 * Called when the user toggles the check button.
 * Reads the toggle state (0 or 1) and writes it to the xconf node as an enum.
 */
static void
gconf_edit_bool_cb(GtkToggleButton *w, xconf *xc)
{
    int i;

    i = gtk_toggle_button_get_active(w);   /* 0 = unchecked, 1 = checked */
    DBG("%s=%d\n", xc->name, i);
    xconf_set_enum(xc, i, bool_enum);  /* writes "false" or "true" */
}

/*
 * gconf_edit_boolean -- create a GtkCheckButton bound to a boolean xconf node.
 *
 * Reads the current boolean value from @xc (via bool_enum table), normalises
 * it, creates a check button with @text label, and sets its initial state.
 *
 * Parameters:
 *   b    - gconf_block to notify on change (may be NULL).
 *   xc   - the xconf node to read/write (stores "true" / "false").
 *   text - label text for the check button.
 *
 * Returns: the new GtkCheckButton widget.
 */
GtkWidget *
gconf_edit_boolean(gconf_block *b, xconf *xc, gchar *text)
{
    gint i = 0;
    GtkWidget *w;

    xconf_get_enum(xc, &i, bool_enum);    /* read current bool value */
    xconf_set_enum(xc, i, bool_enum);     /* normalise */
    w = gtk_check_button_new_with_label(text);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), i);

    g_signal_connect(G_OBJECT(w), "toggled",
        G_CALLBACK(gconf_edit_bool_cb), xc);
    if (b && b->cb)
    {
        g_signal_connect_swapped(G_OBJECT(w), "toggled",
            G_CALLBACK(b->cb), b);
    }

    return w;
}


/*********************************************************
 * Edit color
 *********************************************************/

/*
 * gconf_edit_color_cb -- "color-set" callback for a color button.
 *
 * Called when the user selects a new color (or alpha) in the dialog.
 * Converts the GdkColor to an RRGGBB hex string and stores it in @xc.
 * If the button has an associated alpha xconf node, converts the 16-bit
 * alpha (0..FFFF) to 8-bit (0..FF) and stores it as an integer.
 */
static void
gconf_edit_color_cb(GtkColorButton *w, xconf *xc)
{
    GdkColor c;
    xconf *xc_alpha;

    gtk_color_button_get_color(GTK_COLOR_BUTTON(w), &c);
    xconf_set_value(xc, gdk_color_to_RRGGBB(&c));   /* store as "#RRGGBB" */
    if ((xc_alpha = g_object_get_data(G_OBJECT(w), "alpha")))
    {
        /* alpha from GTK is 0..0xFFFF; convert to 0..0xFF for xconf */
        guint16 a = gtk_color_button_get_alpha(GTK_COLOR_BUTTON(w));
        a >>= 8;   /* 16-bit → 8-bit */
        xconf_set_int(xc_alpha, (int) a);
    }
}

/*
 * gconf_edit_color -- create a GtkColorButton bound to color (+ optional alpha) xconf nodes.
 *
 * Reads the current color from @xc_color (an RRGGBB string), parses it,
 * and creates a GtkColorButton.  If @xc_alpha is provided, reads the 8-bit
 * alpha, scales it to the 16-bit range GTK expects, and enables the alpha
 * channel on the button.
 *
 * Parameters:
 *   b         - gconf_block to notify on change (may be NULL).
 *   xc_color  - xconf node storing the color as an RRGGBB string.
 *   xc_alpha  - xconf node storing the alpha as an integer 0..255 (may be NULL).
 *
 * Returns: the new GtkColorButton widget.
 *
 * Note: @xc_alpha is stashed on the widget via g_object_set_data() for
 *   retrieval in the callback.  @xc_alpha must remain valid for the widget lifetime.
 */
GtkWidget *
gconf_edit_color(gconf_block *b, xconf *xc_color, xconf *xc_alpha)
{

    GtkWidget *w;
    GdkColor c;

    gdk_color_parse(xconf_get_value(xc_color), &c);  /* parse current color */

    w = gtk_color_button_new();
    gtk_color_button_set_color(GTK_COLOR_BUTTON(w), &c);
    if (xc_alpha)
    {
        gint a;

        xconf_get_int(xc_alpha, &a);
        a <<= 8; /* scale 0..FF to 0..FFFF for GTK's 16-bit alpha */
        gtk_color_button_set_alpha(GTK_COLOR_BUTTON(w), (guint16) a);
        /* stash alpha node on widget for retrieval in callback */
        g_object_set_data(G_OBJECT(w), "alpha", xc_alpha);
    }
    /* enable alpha channel in the color picker dialog only if alpha node present */
    gtk_color_button_set_use_alpha(GTK_COLOR_BUTTON(w),
        xc_alpha != NULL);

    g_signal_connect(G_OBJECT(w), "color-set",
        G_CALLBACK(gconf_edit_color_cb), xc_color);
    if (b && b->cb)
    {
        g_signal_connect_swapped(G_OBJECT(w), "color-set",
            G_CALLBACK(b->cb), b);
    }

    return w;
}
