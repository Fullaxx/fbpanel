/*
 * image.c -- fbpanel static image plugin.
 *
 * Displays a static image (loaded from a file path) scaled to fit the panel
 * height (or width for vertical panels).  Optionally shows a tooltip.
 *
 * The image is rendered as a GtkImage from a GdkPixmap+GdkBitmap pair
 * (GDK2-style rendering with optional transparency mask).
 *
 * Config keys:
 *   image   = /path/to/image.png   File path to display (required).
 *                                   "~" at start is expanded to $HOME.
 *   tooltip = "Some text"          Tooltip markup (optional).
 *
 * Known limitations:
 *   - Does not auto-reload on icon theme change (uses a file path, not icon name).
 *   - Does not respond to panel size changes after construction.
 *   - The destructor calls gtk_widget_destroy(img->mainw) even though mainw is
 *     a child of p->pwid and will be destroyed automatically when pwid is
 *     destroyed — this causes a double-destroy (see BUGS_AND_ISSUES.md).
 */

#include <stdlib.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"


/*
 * image_priv -- private data for one image plugin instance.
 *
 * Fields:
 *   plugin  - base plugin_instance (MUST be first).
 *   pix     - GdkPixmap rendered from the scaled pixbuf; owned by image_priv;
 *             freed in destructor with g_object_unref().
 *   mask    - GdkBitmap transparency mask (may be NULL if image has no alpha);
 *             freed in destructor with g_object_unref().
 *   mainw   - GtkEventBox containing the GtkImage; child of p->pwid.
 *             WARNING: destructor calls gtk_widget_destroy(mainw) which is
 *             redundant since pwid destruction also destroys mainw.
 */
typedef struct {
    plugin_instance plugin;  /* must be first */
    GdkPixmap *pix;          /* rendered pixmap (owned; g_object_unref in dtor) */
    GdkBitmap *mask;         /* alpha mask (owned; may be NULL; g_object_unref in dtor) */
    GtkWidget *mainw;        /* GtkEventBox child of pwid */
} image_priv;


/*
 * image_destructor -- clean up the image plugin.
 *
 * Releases the GdkPixmap and GdkBitmap refs.
 *
 * BUG: gtk_widget_destroy(img->mainw) is called here, but mainw is already
 *      a child of p->pwid.  The panel destroys pwid (and thus mainw) AFTER
 *      this destructor returns.  Calling gtk_widget_destroy on mainw here
 *      causes it to be destroyed twice — the first destroy removes it from
 *      its parent, then when the panel destroys pwid, GTK may warn or crash.
 *      Fix: remove the gtk_widget_destroy(img->mainw) call.
 */
static void
image_destructor(plugin_instance *p)
{
    image_priv *img = (image_priv *) p;

    ENTER;
    gtk_widget_destroy(img->mainw);   /* BUG: double-destroy; remove this line */
    if (img->mask)
        g_object_unref(img->mask);    /* release transparency mask */
    if (img->pix)
        g_object_unref(img->pix);     /* release rendered pixmap */
    RET();
}

/*
 * image_constructor -- initialise the image plugin.
 *
 * Reads "image" (file path) and "tooltip" from config.
 * Loads the image file, scales it to fit the panel dimension, renders it
 * into a GdkPixmap+GdkBitmap pair, and creates a GtkImage from that pair.
 * If the file cannot be loaded, shows a "?" label as a fallback.
 *
 * Parameters:
 *   p - the plugin_instance.
 *
 * Returns: 1 (always; fallback label is shown if image fails to load).
 *
 * Memory:
 *   fname is returned by expand_tilda (caller must g_free).
 *   gp (original pixbuf) is unref'd after scaling.
 *   gps (scaled pixbuf) is unref'd after rendering to GdkPixmap.
 *   img->pix and img->mask are owned by image_priv until destructor.
 *   tooltip is returned by XCG as a non-owning str pointer — but then
 *   g_free(tooltip) is called, which is INCORRECT (do not free xc-owned strings).
 *   Fix: use XCG with strdup type to get an owned copy first.
 */
static int
image_constructor(plugin_instance *p)
{
    gchar *tooltip, *fname;
    image_priv *img;
    GdkPixbuf *gp, *gps;
    GtkWidget *wid;
    GError *err = NULL;

    ENTER;
    img = (image_priv *) p;
    tooltip = fname = 0;
    XCG(p->xc, "image", &fname, str);        /* read file path (non-owning) */
    XCG(p->xc, "tooltip", &tooltip, str);    /* read tooltip (non-owning) */
    fname = expand_tilda(fname);              /* expand "~" prefix; returns g_malloc'd copy */

    /* Create an event box to host the image (allows tooltip and potential click handling) */
    img->mainw = gtk_event_box_new();
    gtk_widget_show(img->mainw);

    /* Load the image file */
    gp = gdk_pixbuf_new_from_file(fname, &err);
    if (!gp) {
        /* File not found or unreadable — show a "?" placeholder label */
        g_warning("image: can't read image %s\n", fname);
        wid = gtk_label_new("?");
    } else {
        float ratio;

        /* Scale the image to fit the panel dimension (panel height for horizontal,
         * panel width for vertical), preserving aspect ratio.
         * Subtract 2px from dimension to leave a small border.
         * Uses panel->ah (preferred height) and panel->aw (preferred width). */
        ratio = (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) ?
            (float) (p->panel->ah - 2) / (float) gdk_pixbuf_get_height(gp)
            : (float) (p->panel->aw - 2) / (float) gdk_pixbuf_get_width(gp);

        /* Scale using high-quality HYPER interpolation */
        gps = gdk_pixbuf_scale_simple(gp,
              (int)(ratio * (float) gdk_pixbuf_get_width(gp)),
              (int)(ratio * (float) gdk_pixbuf_get_height(gp)),
              GDK_INTERP_HYPER);

        /* Render pixbuf to GdkPixmap + alpha mask (threshold: 127/255) */
        gdk_pixbuf_render_pixmap_and_mask(gps, &img->pix, &img->mask, 127);

        g_object_unref(gp);    /* done with original pixbuf */
        g_object_unref(gps);   /* done with scaled pixbuf */

        /* Create a GtkImage from the rendered GdkPixmap */
        wid = gtk_image_new_from_pixmap(img->pix, img->mask);
    }
    gtk_widget_show(wid);
    gtk_container_add(GTK_CONTAINER(img->mainw), wid);   /* add image to event box */
    gtk_container_set_border_width(GTK_CONTAINER(img->mainw), 0);
    g_free(fname);           /* free the expand_tilda result */
    gtk_container_add(GTK_CONTAINER(p->pwid), img->mainw);
    if (tooltip) {
        gtk_widget_set_tooltip_markup(img->mainw, tooltip);
        /* BUG: tooltip is a non-owning pointer from XCG(str); g_free here is wrong.
         * Should use XCG(strdup) to get an owned copy. Freeing here may corrupt
         * the xconf tree's string data. */
        g_free(tooltip);
    }
    RET(1);
}


/* plugin_class descriptor for the image plugin */
static plugin_class class = {
    .count       = 0,
    .type        = "image",
    .name        = "Show Image",
    .version     = "1.0",
    .description = "Dispaly Image and Tooltip",   /* NOTE: typo "Dispaly" in original */
    .priv_size   = sizeof(image_priv),

    .constructor = image_constructor,
    .destructor  = image_destructor,
};
/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
