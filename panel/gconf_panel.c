/*
 * gconf_panel.c -- fbpanel "Panel" preferences tab.
 *
 * Implements the "Panel" notebook page in the fbpanel preferences dialog.
 * The tab contains three gconf_block sections:
 *   1. Geometry  — edge, alignment, width (type+value), height, margins.
 *   2. Properties — dock type, strut, stacking layer.
 *   3. Visual Effects — transparency+color, round corners, autohide.
 *
 * Each section is built with gconf_block helpers (gconf.h) that bind
 * GtkWidgets directly to xconf config nodes.  Changes take effect
 * immediately in the in-memory xconf tree; they are saved to disk only
 * when the user clicks Apply or OK.
 *
 * Public API:
 *   configure(xconf *xc) — show (or re-raise) the preferences dialog.
 *
 * Design notes:
 *   - The dialog is a file-scope static singleton (only one can be open).
 *   - mk_dialog() creates TWO xconf_dup's of the passed oxc:
 *       1st: stored as "oxc" on the dialog (snapshot for comparison).
 *       2nd: stored as xc (working copy passed to all editor widgets).
 *     On Apply, if xc differs from oxc, the new xc is saved and the panel
 *     restarts (gtk_main_quit()).
 *   - On Close/OK/Delete, all gconf_blocks and both xconf copies are freed.
 *
 * Known bugs / limitations:
 *   BUG: gconf_block_free() is NOT called for any block on APPLY — only on
 *     CLOSE/DELETE/OK.  This is correct (blocks live as long as the dialog),
 *     but means blocks accumulate for the lifetime of the dialog.
 *   BUG: mk_dialog() allocates xc = xconf_dup(oxc) TWICE (lines ~372-374).
 *     The first copy is stored as "oxc" (snapshot); the second is xc.
 *     This is correct but confusing — both are separate copies of oxc.
 *   NOTE: dialog is a static global — only one preferences dialog can be
 *     open at a time.  configure() idempotently raises the existing dialog
 *     if called again.
 */

#include "gconf.h"
#include "panel.h"

//#define DEBUGPRN
#include "dbg.h"

/* the single preferences dialog instance (NULL when closed) */
static GtkWidget *dialog;

/* Static references to specific spin/combo widgets for geom_changed() */
static GtkWidget *width_spin, *width_opt;
static GtkWidget *xmargin_spin, *ymargin_spin;
static GtkWidget *allign_opt;

/* One gconf_block per section/subsection of the panel tab */
static gconf_block *gl_block;       /* outer container for the entire panel tab */
static gconf_block *geom_block;     /* Geometry section */
static gconf_block *prop_block;     /* Properties section */
static gconf_block *effects_block;  /* Visual Effects section */
static gconf_block *color_block;    /* Transparency color sub-block */
static gconf_block *corner_block;   /* Round-corners radius sub-block */
static gconf_block *layer_block;    /* Stacking layer sub-block */
static gconf_block *ah_block;       /* Autohide height sub-block */

/* Extra indent for dependent sub-blocks */
#define INDENT_2 25


GtkWidget *mk_tab_plugins(xconf *xc);   /* defined in gconf_plugins.c */

/*********************************************************
 * panel effects
 *********************************************************/

/*
 * effects_changed -- gconf_block callback for the Effects section.
 *
 * Called whenever a checkbox in effects_block changes.
 * Reads the current values of "transparent", "roundcorners", and "autohide"
 * from the working xconf copy and shows/hides the corresponding sub-blocks.
 *
 * Parameters:
 *   b - the effects_block (b->data is the working xconf* for the global section).
 */
static void
effects_changed(gconf_block *b)
{
    int i;

    ENTER;
    XCG(b->data, "transparent", &i, enum, bool_enum);
    gtk_widget_set_sensitive(color_block->main, i);   /* show color picker only if transparent */
    XCG(b->data, "roundcorners", &i, enum, bool_enum);
    gtk_widget_set_sensitive(corner_block->main, i);  /* show radius spinner only if rounded */
    XCG(b->data, "autohide", &i, enum, bool_enum);
    gtk_widget_set_sensitive(ah_block->main, i);      /* show hidden-height spinner if autohide */

    RET();
}


/*
 * mk_effects_block -- build the "Visual Effects" section of the panel tab.
 *
 * Adds to gl_block:
 *   - "Visual Effects" heading label.
 *   - effects_block containing:
 *       * "Transparency" checkbox + color_block (color+alpha picker, indented).
 *       * "Round corners" checkbox + corner_block (radius spinner, indented).
 *       * "Autohide" checkbox + ah_block (hidden-height spinner, indented).
 *       * "Max Element Height" spinner.
 *
 * Parameters:
 *   xc - the working xconf* for the "global" config section.
 */
static void
mk_effects_block(xconf *xc)
{
    GtkWidget *w;

    ENTER;

    /* section heading */
    w = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(w), 0, 0.5);
    gtk_label_set_markup(GTK_LABEL(w), _("<b>Visual Effects</b>"));
    gconf_block_add(gl_block, w, TRUE);

    /* main effects block; notifies effects_changed when any child changes */
    effects_block = gconf_block_new((GCallback)effects_changed, xc, 10);

    /* Transparency checkbox */
    w = gconf_edit_boolean(effects_block, xconf_get(xc, "transparent"),
        _("Transparency"));
    gconf_block_add(effects_block, w, TRUE);

    /* Color sub-block (indented; visible only when transparent=true) */
    color_block = gconf_block_new(NULL, NULL, INDENT_2);
    w = gtk_label_new(_("Color settings"));
    gconf_block_add(color_block, w, TRUE);
    w = gconf_edit_color(color_block, xconf_get(xc, "tintcolor"),
        xconf_get(xc, "alpha"));
    gconf_block_add(color_block, w, FALSE);

    gconf_block_add(effects_block, color_block->main, TRUE);

    /* Round corners checkbox */
    w = gconf_edit_boolean(effects_block, xconf_get(xc, "roundcorners"),
        _("Round corners"));
    gconf_block_add(effects_block, w, TRUE);

    /* Corner radius sub-block (indented; visible only when roundcorners=true) */
    corner_block = gconf_block_new(NULL, NULL, INDENT_2);
    w = gtk_label_new(_("Radius is "));
    gconf_block_add(corner_block, w, TRUE);
    /* NOTE: uses geom_block as the parent for notification — this seems unintentional;
     * should probably use corner_block or NULL */
    w = gconf_edit_int(geom_block, xconf_get(xc, "roundcornersradius"), 0, 30);
    gconf_block_add(corner_block, w, FALSE);
    w = gtk_label_new(_("pixels"));
    gconf_block_add(corner_block, w, FALSE);
    gconf_block_add(effects_block, corner_block->main, TRUE);

    /* Autohide checkbox */
    w = gconf_edit_boolean(effects_block, xconf_get(xc, "autohide"),
        _("Autohide"));
    gconf_block_add(effects_block, w, TRUE);

    /* Hidden height sub-block (indented; visible only when autohide=true) */
    ah_block = gconf_block_new(NULL, NULL, INDENT_2);
    w = gtk_label_new(_("Height when hidden is "));
    gconf_block_add(ah_block, w, TRUE);
    w = gconf_edit_int(ah_block, xconf_get(xc, "heightwhenhidden"), 0, 10);
    gconf_block_add(ah_block, w, FALSE);
    w = gtk_label_new(_("pixels"));
    gconf_block_add(ah_block, w, FALSE);
    gconf_block_add(effects_block, ah_block->main, TRUE);

    /* Max element height spinner (always visible) */
    w = gconf_edit_int(effects_block, xconf_get(xc, "maxelemheight"), 0, 128);
    gconf_block_add(effects_block, gtk_label_new(_("Max Element Height")), TRUE);
    gconf_block_add(effects_block, w, FALSE);
    gconf_block_add(gl_block, effects_block->main, TRUE);

    /* empty spacing row */
    gconf_block_add(gl_block, gtk_label_new(" "), TRUE);
}

/*********************************************************
 * panel properties
 *********************************************************/

/*
 * prop_changed -- gconf_block callback for the Properties section.
 *
 * Shows/hides the layer_block based on whether "Set stacking layer" is checked.
 *
 * Parameters:
 *   b - the prop_block (b->data is the working xconf* for global section).
 */
static void
prop_changed(gconf_block *b)
{
    int i = 0;

    ENTER;
    XCG(b->data, "setlayer", &i, enum, bool_enum);
    gtk_widget_set_sensitive(layer_block->main, i);  /* show layer combo if setlayer=true */
    RET();
}

/*
 * mk_prop_block -- build the "Properties" section of the panel tab.
 *
 * Adds to gl_block:
 *   - "Properties" heading label.
 *   - prop_block containing:
 *       * "Do not cover by maximised windows" checkbox (setpartialstrut).
 *       * "Set 'Dock' type" checkbox (setdocktype).
 *       * "Set stacking layer" checkbox + layer_block (layer combo, indented).
 *
 * Parameters:
 *   xc - the working xconf* for the "global" config section.
 */
static void
mk_prop_block(xconf *xc)
{
    GtkWidget *w;

    ENTER;

    /* section heading */
    w = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(w), 0, 0.5);
    gtk_label_set_markup(GTK_LABEL(w), _("<b>Properties</b>"));
    gconf_block_add(gl_block, w, TRUE);

    /* properties block; notifies prop_changed when any child changes */
    prop_block = gconf_block_new((GCallback)prop_changed, xc, 10);

    /* strut checkbox */
    w = gconf_edit_boolean(prop_block, xconf_get(xc, "setpartialstrut"),
        _("Do not cover by maximized windows"));
    gconf_block_add(prop_block, w, TRUE);

    /* dock type checkbox */
    w = gconf_edit_boolean(prop_block, xconf_get(xc, "setdocktype"),
        _("Set 'Dock' type"));
    gconf_block_add(prop_block, w, TRUE);

    /* set layer checkbox */
    w = gconf_edit_boolean(prop_block, xconf_get(xc, "setlayer"),
        _("Set stacking layer"));
    gconf_block_add(prop_block, w, TRUE);

    /* Layer sub-block (indented; visible only when setlayer=true) */
    layer_block = gconf_block_new(NULL, NULL, INDENT_2);
    w = gtk_label_new(_("Panel is "));
    gconf_block_add(layer_block, w, TRUE);
    w = gconf_edit_enum(layer_block, xconf_get(xc, "layer"),
        layer_enum);
    gconf_block_add(layer_block, w, FALSE);
    w = gtk_label_new(_("all windows"));
    gconf_block_add(layer_block, w, FALSE);
    gconf_block_add(prop_block, layer_block->main, TRUE);


    gconf_block_add(gl_block, prop_block->main, TRUE);

    /* empty spacing row */
    gconf_block_add(gl_block, gtk_label_new(" "), TRUE);
}

/*********************************************************
 * panel geometry
 *********************************************************/

/*
 * geom_changed -- gconf_block callback for the Geometry section.
 *
 * Called when alignment or width-type changes.  Updates:
 *   - xmargin_spin sensitivity (disabled when centered).
 *   - width_spin sensitivity (disabled when WIDTH_REQUEST).
 *   - width_spin range (0..100 for percent; 0..screen_size for pixels).
 *
 * Parameters:
 *   b - the geom_block (b->data is the working xconf* for global section).
 */
static void
geom_changed(gconf_block *b)
{
    int i, j;

    ENTER;
    i = gtk_combo_box_get_active(GTK_COMBO_BOX(allign_opt));
    /* x margin only makes sense when not centered */
    gtk_widget_set_sensitive(xmargin_spin, (i != ALLIGN_CENTER));
    i = gtk_combo_box_get_active(GTK_COMBO_BOX(width_opt));
    /* width spinner disabled if width is determined by content */
    gtk_widget_set_sensitive(width_spin, (i != WIDTH_REQUEST));
    if (i == WIDTH_PERCENT)
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(width_spin), 0, 100);
    else if (i == WIDTH_PIXEL) {
        /* range is screen size along the panel's axis */
        XCG(b->data, "edge", &j, enum, edge_enum);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(width_spin), 0,
            (j == EDGE_RIGHT || j == EDGE_LEFT)
            ? gdk_screen_height() : gdk_screen_width());
    }
    RET();
}

/*
 * mk_geom_block -- build the "Geometry" section of the panel tab.
 *
 * Adds to gl_block:
 *   - "Geometry" heading label.
 *   - geom_block containing:
 *       * Width: spinner + type combo (percent/pixel/request).
 *       * Height: spinner.
 *       * Edge: combo (top/bottom/left/right).
 *       * Alignment: combo (center/left/right).
 *       * X Margin: spinner.
 *       * Y Margin: spinner.
 *
 * Parameters:
 *   xc - the working xconf* for the "global" config section.
 */
static void
mk_geom_block(xconf *xc)
{
    GtkWidget *w;

    ENTER;

    /* section heading */
    w = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(w), 0, 0.5);
    gtk_label_set_markup(GTK_LABEL(w), _("<b>Geometry</b>"));
    gconf_block_add(gl_block, w, TRUE);

    /* geometry block; notifies geom_changed when any child changes */
    geom_block = gconf_block_new((GCallback)geom_changed, xc, 10);

    /* Width spinner + type combo */
    w = gconf_edit_int(geom_block, xconf_get(xc, "width"), 0, 2300);
    gconf_block_add(geom_block, gtk_label_new(_("Width")), TRUE);
    gconf_block_add(geom_block, w, FALSE);
    width_spin = w;

    w = gconf_edit_enum(geom_block, xconf_get(xc, "widthtype"),
        widthtype_enum);
    gconf_block_add(geom_block, w, FALSE);
    width_opt = w;

    /* Height spinner */
    w = gconf_edit_int(geom_block, xconf_get(xc, "height"), 0, 300);
    gconf_block_add(geom_block, gtk_label_new(_("Height")), TRUE);
    gconf_block_add(geom_block, w, FALSE);

    /* Edge combo (which screen edge) */
    w = gconf_edit_enum(geom_block, xconf_get(xc, "edge"),
        edge_enum);
    gconf_block_add(geom_block, gtk_label_new(_("Edge")), TRUE);
    gconf_block_add(geom_block, w, FALSE);

    /* Alignment combo */
    w = gconf_edit_enum(geom_block, xconf_get(xc, "allign"),
        allign_enum);
    gconf_block_add(geom_block, gtk_label_new(_("Alignment")), TRUE);
    gconf_block_add(geom_block, w, FALSE);
    allign_opt = w;

    /* X Margin spinner */
    w = gconf_edit_int(geom_block, xconf_get(xc, "xmargin"), 0, 300);
    gconf_block_add(geom_block, gtk_label_new(_("X Margin")), TRUE);
    gconf_block_add(geom_block, w, FALSE);
    xmargin_spin = w;

    /* Y Margin spinner (same row as X Margin label — intentional for compactness) */
    w = gconf_edit_int(geom_block, xconf_get(xc, "ymargin"), 0, 300);
    gconf_block_add(geom_block, gtk_label_new(_("Y Margin")), FALSE);
    gconf_block_add(geom_block, w, FALSE);
    ymargin_spin = w;

    gconf_block_add(gl_block, geom_block->main, TRUE);

    /* empty spacing row */
    gconf_block_add(gl_block, gtk_label_new(" "), TRUE);
}

/*
 * mk_tab_global -- build the complete "Panel" notebook tab.
 *
 * Creates a vertical page containing the three gconf_block sections
 * (Geometry, Properties, Visual Effects), then calls each section's
 * geom_changed / effects_changed / prop_changed callback once to initialise
 * widget sensitivity.
 *
 * Parameters:
 *   xc - the "global" sub-node of the working xconf copy.
 *
 * Returns: a GtkVBox widget ready to be inserted as a notebook page.
 */
static GtkWidget *
mk_tab_global(xconf *xc)
{
    GtkWidget *page;

    ENTER;
    page = gtk_vbox_new(FALSE, 1);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gl_block = gconf_block_new(NULL, NULL, 0);
    gtk_box_pack_start(GTK_BOX(page), gl_block->main, FALSE, TRUE, 0);

    mk_geom_block(xc);
    mk_prop_block(xc);
    mk_effects_block(xc);

    gtk_widget_show_all(page);

    /* initialise widget sensitivity based on current config values */
    geom_changed(geom_block);
    effects_changed(effects_block);
    prop_changed(prop_block);

    RET(page);
}

/*
 * mk_tab_profile -- build the "Profile" notebook tab.
 *
 * Displays a text label showing the current profile name and its file path.
 * This is a read-only informational tab.
 *
 * Parameters:
 *   xc - the working xconf copy (not used for display; kept for signature uniformity).
 *
 * Returns: a GtkVBox widget ready to be inserted as a notebook page.
 */
static GtkWidget *
mk_tab_profile(xconf *xc)
{
    GtkWidget *page, *label;
    gchar *s1;

    ENTER;
    page = gtk_vbox_new(FALSE, 1);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    s1 = g_strdup_printf(_("You're using '<b>%s</b>' profile, stored at\n"
            "<tt>%s</tt>"), panel_get_profile(), panel_get_profile_file());
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), s1);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, TRUE, 0);
    g_free(s1);

    gtk_widget_show_all(page);
    RET(page);
}

/*
 * dialog_response_event -- GtkDialog "response" handler.
 *
 * Handles Apply, OK, Close, and Delete (window manager close) responses:
 *
 * Apply / OK:
 *   Compares the working copy (xc) to the snapshot (oxc).  If they differ,
 *   saves the config to disk, updates the snapshot, and restarts the panel
 *   (gtk_main_quit()).
 *
 * Close / OK / Delete:
 *   Destroys the dialog, frees all gconf_blocks, and frees both xconf copies.
 *
 * Parameters:
 *   _dialog - the GtkDialog widget (same as file-scope `dialog`).
 *   rid     - GTK_RESPONSE_APPLY, _OK, _CLOSE, or _DELETE_EVENT.
 *   xc      - the working xconf copy (second dup created in mk_dialog).
 *
 * Note: "oxc" is the snapshot xconf dup stored as GObject data on the dialog.
 *
 * BUG: On APPLY, the gconf_blocks are NOT freed (correct — they must remain
 *   alive while the dialog is open), but a new snapshot (oxc) is allocated
 *   to replace the old one.  The old oxc is correctly freed with xconf_del.
 */
static void
dialog_response_event(GtkDialog *_dialog, gint rid, xconf *xc)
{
    xconf *oxc = g_object_get_data(G_OBJECT(dialog), "oxc");

    ENTER;
    if (rid == GTK_RESPONSE_APPLY ||
        rid == GTK_RESPONSE_OK)
    {
        DBG("apply changes\n");
        if (xconf_cmp(xc, oxc))  /* TRUE = trees differ → save needed */
        {
            xconf_del(oxc, FALSE);             /* free old snapshot */
            oxc = xconf_dup(xc);              /* new snapshot of current state */
            g_object_set_data(G_OBJECT(dialog), "oxc", oxc);
            xconf_save_to_profile(xc);         /* persist to disk */
            gtk_main_quit();                   /* trigger panel restart */
        }
    }
    if (rid == GTK_RESPONSE_DELETE_EVENT ||
        rid == GTK_RESPONSE_CLOSE ||
        rid == GTK_RESPONSE_OK)
    {
        gtk_widget_destroy(dialog);
        dialog = NULL;
        /* free all gconf_blocks */
        gconf_block_free(geom_block);
        gconf_block_free(gl_block);
        gconf_block_free(effects_block);
        gconf_block_free(color_block);
        gconf_block_free(corner_block);
        gconf_block_free(layer_block);
        gconf_block_free(prop_block);
        gconf_block_free(ah_block);
        xconf_del(xc, FALSE);    /* free working copy */
        xconf_del(oxc, FALSE);   /* free snapshot */
    }
    RET();
}

/*
 * dialog_cancel -- GtkDialog "delete-event" handler.
 *
 * Called when the user closes the dialog via the window manager (e.g., [X]).
 * Delegates to dialog_response_event with GTK_RESPONSE_CLOSE.
 *
 * Returns: FALSE to allow GTK to destroy the dialog window.
 */
static gboolean
dialog_cancel(GtkDialog *_dialog, GdkEvent *event, xconf *xc)
{
    dialog_response_event(_dialog, GTK_RESPONSE_CLOSE, xc);
    return FALSE;
}

/*
 * mk_dialog -- create the fbpanel preferences dialog.
 *
 * Allocates and configures a GtkDialog with three notebook tabs:
 *   - "Panel"   → mk_tab_global()
 *   - "Plugins" → mk_tab_plugins()
 *   - "Profile" → mk_tab_profile()
 *
 * Parameters:
 *   oxc - the current panel xconf tree (root, NOT just "global").
 *
 * Returns: the new GtkDialog widget (also stored in the file-scope `dialog`).
 *
 * Memory:
 *   Two xconf_dup's of oxc are created:
 *     1st → stored as "oxc" on the dialog via g_object_set_data (snapshot).
 *     2nd → local `xc`, passed to all editor widgets (working copy).
 *   Both are freed in dialog_response_event when the dialog is closed.
 *
 * BUG: `xc` is duplicated twice from `oxc`.  The code is:
 *   xc = xconf_dup(oxc);                           // dup 1 → stored as "oxc"
 *   g_object_set_data(dialog, "oxc", xc);
 *   xc = xconf_dup(oxc);                           // dup 2 → working copy
 *   Both are correct copies of the ORIGINAL oxc passed in.  The naming is
 *   confusing but the behaviour is correct.
 */
static GtkWidget *
mk_dialog(xconf *oxc)
{
    GtkWidget *sw, *nb, *label;
    gchar *name;
    xconf *xc;

    ENTER;
    DBG("creating dialog\n");
    name = g_strdup_printf("fbpanel settings: <%s> profile",
        panel_get_profile());
    dialog = gtk_dialog_new_with_buttons (name,
        NULL,
        GTK_DIALOG_NO_SEPARATOR,
        GTK_STOCK_APPLY,
        GTK_RESPONSE_APPLY,
        GTK_STOCK_OK,
        GTK_RESPONSE_OK,
        GTK_STOCK_CLOSE,
        GTK_RESPONSE_CLOSE,
        NULL);
    g_free(name);
    DBG("connecting signal to %p\n",  dialog);

    /* create snapshot copy (oxc) — used for change detection */
    xc = xconf_dup(oxc);
    g_object_set_data(G_OBJECT(dialog), "oxc", xc);  /* snapshot stored on widget */
    /* create working copy (xc) — bound to all editor widgets */
    xc = xconf_dup(oxc);

    g_signal_connect (G_OBJECT(dialog), "response",
        (GCallback) dialog_response_event, xc);
    g_signal_connect (G_OBJECT(dialog), "delete_event",
        (GCallback) dialog_cancel, xc);

    gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);   /* non-modal: panel still usable */
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 500);
    gtk_window_set_icon_from_file(GTK_WINDOW(dialog),
        IMGPREFIX "/logo.png", NULL);

    nb = gtk_notebook_new();
    gtk_notebook_set_show_border (GTK_NOTEBOOK(nb), FALSE);
    gtk_container_add (GTK_CONTAINER (GTK_DIALOG(dialog)->vbox), nb);

    /* Panel tab — geometry, properties, effects */
    sw = mk_tab_global(xconf_get(xc, "global"));
    label = gtk_label_new(_("Panel"));
    gtk_misc_set_padding(GTK_MISC(label), 4, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, label);

    /* Plugins tab — plugin list manager */
    sw = mk_tab_plugins(xc);
    label = gtk_label_new(_("Plugins"));
    gtk_misc_set_padding(GTK_MISC(label), 4, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, label);

    /* Profile tab — informational */
    sw = mk_tab_profile(xc);
    label = gtk_label_new(_("Profile"));
    gtk_misc_set_padding(GTK_MISC(label), 4, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, label);

    gtk_widget_show_all(dialog);
    RET(dialog);
}

/*
 * configure -- show the fbpanel preferences dialog.
 *
 * If the dialog is not yet open, creates it with mk_dialog().
 * If already open, brings it to the foreground.
 *
 * Parameters:
 *   xc - the current xconf root tree (passed to mk_dialog as oxc).
 *
 * Note: Only one preferences dialog can be open at a time (singleton).
 */
void
configure(xconf *xc)
{
    ENTER;
    DBG("dialog %p\n",  dialog);
    if (!dialog)
        dialog = mk_dialog(xc);
    gtk_widget_show(dialog);   /* re-raise if already open */
    RET();
}
