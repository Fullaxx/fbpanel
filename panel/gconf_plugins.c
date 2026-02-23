/*
 * gconf_plugins.c -- fbpanel "Plugins" preferences tab (stub/incomplete).
 *
 * Implements the "Plugins" notebook page in the fbpanel preferences dialog.
 * The page shows a GtkTreeView listing the currently loaded plugins, with
 * Add / Edit / Delete / Move buttons below.
 *
 * IMPORTANT: This is an INCOMPLETE/STUB implementation.  The following
 * functionality is NOT yet wired up:
 *   - The "Name" column always displays "Martin Heidegger" (placeholder
 *     text that was never replaced with real plugin names).
 *   - The Add, Edit, Delete, Move buttons are created but NOT connected
 *     to any signal handlers — they are non-functional.
 *   - The tree model is never updated after creation.
 *
 * Known bugs:
 *   BUG: NAME_COL always shows "Martin Heidegger" instead of the plugin's
 *     display name.  Fix: look up plugin_class->name via class_get(type).
 *   BUG: "Type" column header is hardcoded English (not translated via _()).
 *   BUG: Add / Edit / Delete / Move buttons have no signal handlers.
 *   BUG: store and tree are file-scope globals — cannot support multiple
 *     dialog instances or re-opening.
 *   BUG: bbox is a file-scope global, not cleaned up between dialog opens.
 */

#include "gconf.h"
#include "panel.h"

//#define DEBUGPRN
#include "dbg.h"

/*
 * Column indices for the GtkTreeStore.
 *   TYPE_COL — the plugin type string (e.g. "taskbar", "dclock").
 *   NAME_COL — the plugin display name (stub: always "Martin Heidegger").
 */
enum
{
    TYPE_COL,
    NAME_COL,
    N_COLUMNS
};

/* File-scope globals (stub: not cleaned up between dialog open/close) */
GtkTreeStore *store;  /* data model for the plugin list */
GtkWidget *tree;      /* the GtkTreeView widget */
GtkWidget *bbox;      /* button box for Edit/Delete/Move (sensitivity-controlled) */

/*
 * mk_model -- populate the GtkTreeStore from the plugin list in the xconf tree.
 *
 * Iterates over all "plugin" blocks in @xc, reads the "type" key from each,
 * and inserts a row into @store.
 *
 * Parameters:
 *   xc - the root xconf node (contains "plugin" sub-trees).
 *
 * BUG: NAME_COL is set to the hardcoded string "Martin Heidegger" for every
 *   plugin instead of the plugin's actual display name.  This is clearly
 *   placeholder/debug code that was never completed.
 */
static void
mk_model(xconf *xc)
{
    GtkTreeIter iter;
    xconf *pxc;
    int i;
    gchar *type;

    /* two string columns: plugin type and plugin display name */
    store = gtk_tree_store_new(N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING);
    for (i = 0; (pxc = xconf_find(xc, "plugin", i)); i++)
    {
        XCG(pxc, "type", &type, str);   /* read plugin type (non-owning) */
        gtk_tree_store_append(store, &iter, NULL);
        gtk_tree_store_set (store, &iter,
            TYPE_COL, type,
            /* BUG: "Martin Heidegger" is placeholder; should be plugin display name */
            NAME_COL, "Martin Heidegger",
            -1);
    }
}

/*
 * tree_selection_changed_cb -- GtkTreeSelection "changed" callback.
 *
 * Called when the user clicks on a row in the plugin list.
 * Enables the button box (Edit/Delete/Move) when a row is selected;
 * disables it when nothing is selected.
 *
 * Parameters:
 *   selection - the GtkTreeSelection that changed.
 *   data      - unused.
 */
static void
tree_selection_changed_cb(GtkTreeSelection *selection, gpointer data)
{
    GtkTreeIter iter;
    GtkTreeModel *model;
    gchar *type;
    gboolean sel;

    sel = gtk_tree_selection_get_selected(selection, &model, &iter);
    if (sel)
    {
        /* retrieve and print the selected plugin type (debug only) */
        gtk_tree_model_get(model, &iter, TYPE_COL, &type, -1);
        g_print("%s\n", type);
        g_free(type);   /* gtk_tree_model_get returns a g_strdup'd copy */
    }
    /* enable/disable Edit/Delete/Move buttons based on selection state */
    gtk_widget_set_sensitive(bbox, sel);
}

/*
 * mk_view -- create the GtkTreeView for the plugin list.
 *
 * Creates a GtkTreeView backed by the file-scope @store, with a single
 * "Type" column showing the plugin type strings.
 * Connects tree_selection_changed_cb to update bbox sensitivity.
 *
 * Returns: the GtkTreeView widget (also stored in file-scope @tree).
 *
 * Note: The "Name" column is defined in the model but not displayed here.
 * BUG: Column header "Type" is not translated.
 */
static GtkWidget *
mk_view()
{
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkTreeSelection *select;

    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Type",   /* BUG: untranslated */
        renderer, "text", TYPE_COL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

    select = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
    g_signal_connect(G_OBJECT(select), "changed",
        G_CALLBACK(tree_selection_changed_cb), NULL);
    return tree;
}

/*
 * mk_buttons -- create the Add / Edit / Delete / Move button bar.
 *
 * Builds a horizontal box containing:
 *   - "Add" button (always sensitive).
 *   - A sub-box (bbox) containing Edit, Delete, Down, Up buttons.
 *     bbox is initially insensitive; becomes sensitive when a row is selected.
 *
 * Returns: the outer horizontal button container.
 *
 * BUG: None of the buttons are connected to any signal handlers.
 *   They are created and displayed but do nothing when clicked.
 */
GtkWidget *
mk_buttons()
{
    GtkWidget *bm, *b, *w;

    bm = gtk_hbox_new(FALSE, 3);   /* outer container */

    /* Add button — always available (no selection required) */
    w = gtk_button_new_from_stock(GTK_STOCK_ADD);
    gtk_box_pack_start(GTK_BOX(bm), w, FALSE, TRUE, 0);
    /* BUG: no "clicked" signal handler connected */

    /* Button box for selection-dependent actions */
    b = gtk_hbox_new(FALSE, 3);
    gtk_box_pack_start(GTK_BOX(bm), b, FALSE, TRUE, 0);
    bbox = b;
    gtk_widget_set_sensitive(bbox, FALSE);   /* disabled until a row is selected */

    w = gtk_button_new_from_stock(GTK_STOCK_EDIT);
    gtk_box_pack_start(GTK_BOX(b), w, FALSE, TRUE, 0);
    /* BUG: no "clicked" signal handler */

    w = gtk_button_new_from_stock(GTK_STOCK_DELETE);
    gtk_box_pack_start(GTK_BOX(b), w, FALSE, TRUE, 0);
    /* BUG: no "clicked" signal handler */

    w = gtk_button_new_from_stock(GTK_STOCK_GO_DOWN);
    gtk_box_pack_start(GTK_BOX(b), w, FALSE, TRUE, 0);
    /* BUG: no "clicked" signal handler */

    w = gtk_button_new_from_stock(GTK_STOCK_GO_UP);
    gtk_box_pack_start(GTK_BOX(b), w, FALSE, TRUE, 0);
    /* BUG: no "clicked" signal handler */

    return bm;
}

/*
 * mk_tab_plugins -- build the "Plugins" notebook tab.
 *
 * Creates the plugin list view and button bar and packs them into a VBox.
 * Called from gconf_panel.c:mk_dialog().
 *
 * Parameters:
 *   xc - the root xconf node (contains "plugin" sub-trees for mk_model()).
 *
 * Returns: a GtkVBox widget ready to be inserted as a notebook page.
 *
 * NOTE: This is a stub; the plugin list is read-only and buttons do nothing.
 */
GtkWidget *
mk_tab_plugins(xconf *xc)
{
    GtkWidget *page, *w;

    ENTER;
    page = gtk_vbox_new(FALSE, 1);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    mk_model(xc);    /* populate the GtkTreeStore from xconf */

    w = mk_view();   /* create the TreeView */
    gtk_box_pack_start(GTK_BOX(page), w, TRUE, TRUE, 0);
    w = mk_buttons();   /* create the button bar */
    gtk_box_pack_start(GTK_BOX(page), w, FALSE, TRUE, 0);

    gtk_widget_show_all(page);
    RET(page);
}
