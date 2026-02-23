#ifndef FBWIDGETS_H
#define FBWIDGETS_H

/*
 * fbwidgets.h -- GTK widget factory for fbpanel.
 *
 * Declares the GtkIconTheme singleton, color utility functions, and the
 * fb_pixbuf / fb_image / fb_button widget factory API.
 *
 * Extracted from misc.h.  Include this header (or misc.h, which includes it)
 * before calling any fb_pixbuf_new / fb_image_new / fb_button_new functions.
 */

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* Default GTK icon theme singleton; set once by fb_init() in misc.c.
 * GTK owns this object; do NOT g_object_ref() or g_object_unref() it. */
extern GtkIconTheme *icon_theme;

/* -----------------------------------------------------------------------
 * Button spacing / color utilities
 * ----------------------------------------------------------------------- */

/* Measure the minimum size requisition of a named GtkButton. */
void get_button_spacing(GtkRequisition *req, GtkContainer *parent, gchar *name);

/* Convert a GdkColor to a packed 24-bit 0x00RRGGBB integer. */
guint32 gcolor2rgb24(GdkColor *color);

/* Convert a GdkColor to a static "#RRGGBB" hex string.
 * WARNING: returns a pointer to a static buffer – not re-entrant. */
gchar *gdk_color_to_RRGGBB(GdkColor *color);

/* -----------------------------------------------------------------------
 * Pixbuf / image / button widget factory
 * ----------------------------------------------------------------------- */

/* Load a GdkPixbuf from an icon name or file path, with optional fallback. */
GdkPixbuf *fb_pixbuf_new(gchar *iname, gchar *fname, int width, int height,
        gboolean use_fallback);

/* Create a GtkImage with automatic icon-theme-change tracking. */
GtkWidget *fb_image_new(gchar *iname, gchar *fname, int width, int height);

/* Create a composite hover/press button widget (GtkBgBox + fb_image). */
GtkWidget *fb_button_new(gchar *iname, gchar *fname, int width, int height,
        gulong hicolor, gchar *name);

#endif /* FBWIDGETS_H */
