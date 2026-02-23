/*
 * launchbar.c -- fbpanel application launcher bar plugin.
 *
 * Displays a row of icon buttons, each launching a command when clicked.
 * Supports drag-and-drop: dropping a URI or a Mozilla-style URL onto a
 * button appends the path/URL to that button's action command and runs it.
 *
 * Configuration (in the "button" xconf sub-blocks):
 *   image   - path to a pixmap file (expanded with expand_tilda).
 *   icon    - named icon (from icon theme).
 *   action  - shell command to execute on click (expanded with expand_tilda).
 *   tooltip - tooltip markup for the button.
 *
 * Layout:
 *   pwid → GtkAlignment → GtkBar → N × fb_button
 *
 *   GtkBar is the custom multi-row bar widget; launchbar_size_alloc
 *   recalculates the number of rows/columns (dimension) when the widget
 *   is resized.
 *
 * Drag-and-drop:
 *   Accepts text/uri-list (whitespace-separated URIs) and text/x-moz-url
 *   (UTF-16 encoded "URL\nTitle").  Each received URI is appended to the
 *   button's action string before spawning the command.
 *
 * Fixed bugs:
 *   Fixed (BUG-003): Removed explicit gtk_widget_destroy(lb->box) from
 *     launchbar_destructor.  The panel framework destroys p->pwid (and thus
 *     lb->box) automatically after the destructor returns.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>


#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "gtkbgbox.h"
#include "run.h"
#include "gtkbar.h"

//#define DEBUGPRN
#include "dbg.h"


/* Cursor states (currently unused; kept for future DnD cursor feedback) */
typedef enum {
    CURSOR_STANDARD,
    CURSOR_DND
} CursorType;

/* DnD target type indices (match target_table order) */
enum {
    TARGET_URILIST,
    TARGET_MOZ_URL,
    TARGET_UTF8_STRING,
    TARGET_STRING,
    TARGET_TEXT,
    TARGET_COMPOUND_TEXT
};

/* Accepted DnD target formats, in preference order */
static const GtkTargetEntry target_table[] = {
    { "text/uri-list", 0, TARGET_URILIST},
    { "text/x-moz-url", 0, TARGET_MOZ_URL},
    { "UTF8_STRING", 0, TARGET_UTF8_STRING },
    { "COMPOUND_TEXT", 0, 0 },
    { "TEXT",          0, 0 },
    { "STRING",        0, 0 }
};

struct launchbarb;  /* forward declaration (unused; kept for legacy) */

/*
 * btn -- per-button state.
 *
 * lb     - back-pointer to the containing launchbar_priv.
 * action - shell command to execute (g_strdup'd; freed in destructor).
 */
typedef struct btn {
    struct launchbar_priv *lb;
    gchar *action;
} btn;

#define MAXBUTTONS 40   /* hard limit on number of launcher buttons */

/*
 * launchbar_priv -- private state for one launchbar plugin instance.
 *
 * plugin               - embedded plugin_instance (MUST be first).
 * box                  - GtkBar holding all buttons.
 * btns[MAXBUTTONS]     - per-button data (action strings etc.).
 * btn_num              - number of buttons currently configured.
 * iconsize             - icon pixel size (derived from panel->max_elem_height).
 * discard_release_event - set when Ctrl+RMB fires to suppress the matching release.
 */
typedef struct launchbar_priv {
    plugin_instance plugin;
    GtkWidget *box;
    btn btns[MAXBUTTONS];
    int btn_num;
    int iconsize;
    unsigned int discard_release_event : 1;
} launchbar_priv;


/*
 * my_button_pressed -- "button-press-event" / "button-release-event" handler.
 *
 * Runs b->action via run_app() on a left button release when the pointer
 * is still inside the button.
 *
 * Ctrl+RMB press is silently consumed and the matching release is
 * discarded (allows the panel to receive the Ctrl+RMB for its own menu).
 *
 * Parameters:
 *   widget - the fb_button widget.
 *   event  - the button event.
 *   b      - the btn struct for this button.
 *
 * Returns: TRUE to stop propagation (except Ctrl+RMB press → FALSE).
 */
static gboolean
my_button_pressed(GtkWidget *widget, GdkEventButton *event, btn *b )
{
    ENTER;
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
        && event->state & GDK_CONTROL_MASK)
    {
        /* Ctrl+RMB: pass through to panel, and eat the coming release */
        b->lb->discard_release_event = 1;
        RET(FALSE);
    }
    if (event->type == GDK_BUTTON_RELEASE && b->lb->discard_release_event)
    {
        /* eat the release that paired with the Ctrl+RMB above */
        b->lb->discard_release_event = 0;
        RET(TRUE);
    }
    g_assert(b != NULL);
    if (event->type == GDK_BUTTON_RELEASE)
    {
        /* fire only if pointer is still inside the button bounds */
        if ((event->x >=0 && event->x < widget->allocation.width)
            && (event->y >=0 && event->y < widget->allocation.height))
        {
            run_app(b->action);
        }
    }
    RET(TRUE);
}

/*
 * launchbar_destructor -- free all launchbar resources.
 *
 * Frees each button's action string.
 *
 * Parameters:
 *   p - plugin_instance.
 *
 * Note: lb->box is a child of p->pwid; the framework destroys p->pwid
 *   (and all its children) after this destructor returns, so no explicit
 *   gtk_widget_destroy(lb->box) is needed here.
 */
static void
launchbar_destructor(plugin_instance *p)
{
    launchbar_priv *lb = (launchbar_priv *) p;
    int i;

    ENTER;
    for (i = 0; i < lb->btn_num; i++)
        g_free(lb->btns[i].action);     /* action was g_strdup'd by expand_tilda */

    RET();
}


/*
 * drag_data_received_cb -- "drag_data_received" handler for launcher buttons.
 *
 * Appends each dragged URI / URL to the button's action string and spawns it.
 *
 * text/uri-list:  whitespace-separated URIs; each is converted from URI to
 *   filename with g_filename_from_uri() before appending.
 * text/x-moz-url: UTF-16 "URL\nTitle"; only the URL part (before the first \n)
 *   is appended.
 *
 * Parameters:
 *   widget - the destination button widget.
 *   context - DnD context (used for gtk_drag_finish on error).
 *   x, y   - drop coordinates (unused).
 *   sd     - selection data containing the dragged content.
 *   info   - target type index (matches target_table enum).
 *   time   - event timestamp.
 *   b      - btn struct for this button.
 */
static void
drag_data_received_cb (GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    GtkSelectionData *sd,
    guint info,
    guint time,
    btn *b)
{
    gchar *s, *str, *tmp, *tok, *tok2;

    ENTER;
    if (sd->length <= 0)
        RET();
    DBG("uri drag received: info=%d/%s len=%d data=%s\n",
         info, target_table[info].target, sd->length, sd->data);
    if (info == TARGET_URILIST)
    {
        /* text/uri-list: whitespace-separated list of URIs */
        s = g_strdup((gchar *)sd->data);
        str = g_strdup(b->action);
        for (tok = strtok(s, "\n \t\r"); tok; tok = strtok(NULL, "\n \t\r"))
        {
            tok2 = g_filename_from_uri(tok, NULL, NULL);
            /* use the local filename if conversion succeeded, else use raw URI */
            tmp = g_strdup_printf("%s '%s'", str, tok2 ? tok2 : tok);
            g_free(str);
            g_free(tok2);
            str = tmp;
        }
        DBG("cmd=<%s>\n", str);
        g_spawn_command_line_async(str, NULL);
        g_free(str);
        g_free(s);
    }
    else if (info == TARGET_MOZ_URL)
    {
        /* text/x-moz-url: UTF-16 "URL\nTitle" — use URL part only */
        gchar *utf8, *tmp;

	utf8 = g_utf16_to_utf8((gunichar2 *) sd->data, (glong) sd->length,
              NULL, NULL, NULL);
        tmp = utf8 ? strchr(utf8, '\n') : NULL;
	if (!tmp)
        {
            ERR("Invalid UTF16 from text/x-moz-url target");
            g_free(utf8);
            gtk_drag_finish(context, FALSE, FALSE, time);
            RET();
	}
	*tmp = '\0';   /* terminate at the \n; everything before is the URL */
        tmp = g_strdup_printf("%s %s", b->action, utf8);
        g_spawn_command_line_async(tmp, NULL);
        DBG("%s %s\n", b->action, utf8);
        g_free(utf8);
        g_free(tmp);
    }
    RET();
}

/*
 * read_button -- parse one "button" xconf block and create a launcher button.
 *
 * Reads image/icon/action/tooltip from @xc, creates an fb_button, connects
 * event and DnD handlers, and packs it into lb->box.
 *
 * Parameters:
 *   p  - plugin_instance (cast to launchbar_priv*).
 *   xc - xconf node for one "button" block.
 *
 * Returns: 1 on success, 0 if the button limit was reached.
 *
 * Memory notes:
 *   action: expand_tilda() returns a g_strdup'd copy → stored in btns[].action,
 *     freed in launchbar_destructor.
 *   fname:  expand_tilda() returns a g_strdup'd copy → g_free'd at end.
 *   iname:  XCG with str → non-owning pointer into xconf tree → NOT freed.
 *   tooltip: XCG with str → non-owning → NOT freed (used only during setup).
 */
static int
read_button(plugin_instance *p, xconf *xc)
{
    launchbar_priv *lb = (launchbar_priv *) p;
    gchar *iname, *fname, *tooltip, *action;
    GtkWidget *button;

    ENTER;
    if (lb->btn_num >= MAXBUTTONS)
    {
        ERR("launchbar: max number of buttons (%d) was reached."
            "skipping the rest\n", lb->btn_num );
        RET(0);
    }
    iname = tooltip = fname = action = NULL;
    XCG(xc, "image",   &fname,   str);   /* non-owning pointer into xconf */
    XCG(xc, "icon",    &iname,   str);   /* non-owning pointer into xconf */
    XCG(xc, "action",  &action,  str);   /* non-owning pointer into xconf */
    XCG(xc, "tooltip", &tooltip, str);   /* non-owning pointer into xconf */

    action = expand_tilda(action);   /* returns g_strdup'd copy */
    fname  = expand_tilda(fname);    /* returns g_strdup'd copy */
    /* iname is not expand_tilda'd — stays as non-owning xconf pointer */

    button = fb_button_new(iname, fname, lb->iconsize,
        lb->iconsize, 0x202020, NULL);

    /* connect both press and release so we can filter Ctrl+RMB */
    g_signal_connect (G_OBJECT (button), "button-release-event",
          G_CALLBACK (my_button_pressed), (gpointer) &lb->btns[lb->btn_num]);
    g_signal_connect (G_OBJECT (button), "button-press-event",
          G_CALLBACK (my_button_pressed), (gpointer) &lb->btns[lb->btn_num]);

    GTK_WIDGET_UNSET_FLAGS (button, GTK_CAN_FOCUS);
    /* configure button as DnD destination for all accepted target types */
    gtk_drag_dest_set (GTK_WIDGET(button),
        GTK_DEST_DEFAULT_ALL,
        target_table, G_N_ELEMENTS (target_table),
        GDK_ACTION_COPY);
    g_signal_connect (G_OBJECT(button), "drag_data_received",
        G_CALLBACK (drag_data_received_cb),
        (gpointer) &lb->btns[lb->btn_num]);

    gtk_box_pack_start(GTK_BOX(lb->box), button, FALSE, FALSE, 0);
    gtk_widget_show(button);

    if (p->panel->transparent)
        gtk_bgbox_set_background(button, BG_INHERIT,
            p->panel->tintcolor, p->panel->alpha);
    gtk_widget_set_tooltip_markup(button, tooltip);

    g_free(fname);   /* fname was g_strdup'd by expand_tilda */
    /* iname: NOT freed — non-owning pointer from XCG str */
    //g_free(iname);   /* correctly commented out; iname is non-owning */
    DBG("here\n");

    lb->btns[lb->btn_num].action = action;   /* transfer ownership of action */
    lb->btns[lb->btn_num].lb     = lb;
    lb->btn_num++;

    RET(1);
}

/*
 * launchbar_size_alloc -- "size-allocate" handler on the GtkAlignment.
 *
 * Recalculates how many icon rows (horizontal panel) or columns (vertical
 * panel) fit in the current allocation, and updates the GtkBar dimension.
 *
 * Parameters:
 *   widget - the GtkAlignment (unused; we use lb->iconsize instead).
 *   a      - new allocation.
 *   lb     - launchbar_priv instance.
 */
static void
launchbar_size_alloc(GtkWidget *widget, GtkAllocation *a,
    launchbar_priv *lb)
{
    int dim;

    ENTER;
    if (lb->plugin.panel->orientation == GTK_ORIENTATION_HORIZONTAL)
        dim = a->height / lb->iconsize;   /* rows for a horizontal panel */
    else
        dim = a->width / lb->iconsize;    /* columns for a vertical panel */
    DBG("width=%d height=%d iconsize=%d -> dim=%d\n",
        a->width, a->height, lb->iconsize, dim);
    gtk_bar_set_dimension(GTK_BAR(lb->box), dim);
    RET();
}

/*
 * launchbar_constructor -- initialise the launchbar plugin.
 *
 * Builds the widget hierarchy:
 *   p->pwid → GtkAlignment → GtkBar (lb->box) → N × fb_button
 *
 * The custom GTK RC string removes button borders/padding for a clean look.
 * Icon size defaults to panel->max_elem_height.
 * Iterates over all "button" sub-blocks in p->xc and calls read_button().
 *
 * Parameters:
 *   p - plugin_instance allocated by panel framework.
 *
 * Returns: 1 (always succeeds; missing buttons are silently skipped).
 */
static int
launchbar_constructor(plugin_instance *p)
{
    launchbar_priv *lb;
    int i;
    xconf *pxc;
    GtkWidget *ali;
    /* RC string removes focus rings and button borders for a clean icon look */
    static gchar *launchbar_rc = "style 'launchbar-style'\n"
        "{\n"
        "GtkWidget::focus-line-width = 0\n"
        "GtkWidget::focus-padding = 0\n"
        "GtkButton::default-border = { 0, 0, 0, 0 }\n"
        "GtkButton::default-outside-border = { 0, 0, 0, 0 }\n"
        "}\n"
        "widget '*' style 'launchbar-style'";

    ENTER;
    lb = (launchbar_priv *) p;
    lb->iconsize = p->panel->max_elem_height;
    DBG("iconsize=%d\n", lb->iconsize);

    gtk_widget_set_name(p->pwid, "launchbar");
    gtk_rc_parse_string(launchbar_rc);

    /* GtkAlignment centres the icon bar within the available space */
    ali = gtk_alignment_new(0.5, 0.5, 0, 0);
    g_signal_connect(G_OBJECT(ali), "size-allocate",
        (GCallback) launchbar_size_alloc, lb);
    gtk_container_set_border_width(GTK_CONTAINER(ali), 0);
    gtk_container_add(GTK_CONTAINER(p->pwid), ali);

    /* GtkBar lays out buttons in a grid; initial dimension is 1 row/column */
    lb->box = gtk_bar_new(p->panel->orientation, 0,
        lb->iconsize, lb->iconsize);
    gtk_container_add(GTK_CONTAINER(ali), lb->box);
    gtk_container_set_border_width(GTK_CONTAINER (lb->box), 0);
    gtk_widget_show_all(ali);

    /* iterate over all "button" sub-blocks in the plugin config */
    for (i = 0; (pxc = xconf_find(p->xc, "button", i)); i++)
        read_button(p, pxc);
    RET(1);
}

static plugin_class class = {
    .count       = 0,
    .type        = "launchbar",
    .name        = "Launchbar",
    .version     = "1.0",
    .description = "Bar with application launchers",
    .priv_size   = sizeof(launchbar_priv),

    .constructor = launchbar_constructor,
    .destructor  = launchbar_destructor,
};
/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
