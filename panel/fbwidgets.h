#ifndef FBWIDGETS_H
#define FBWIDGETS_H

/**
 * @file
 * @brief GTK widget factory for fbpanel.
 *
 * Declares the GtkIconTheme singleton, color utility functions, and the
 * fb_pixbuf / fb_image / fb_button widget factory API.
 *
 * Extracted from misc.h. Include this header (or misc.h, which includes
 * it) before calling any fb_pixbuf_new() / fb_image_new() / fb_button_new()
 * functions.
 */

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/**
 * @brief Default GTK icon theme singleton; set once by fb_init() in misc.c.
 * @note GTK owns this object; do NOT g_object_ref() or g_object_unref() it.
 */
extern GtkIconTheme *icon_theme;

/* -----------------------------------------------------------------------
 * Button spacing / color utilities
 * ----------------------------------------------------------------------- */

/**
 * @brief Measure the minimum size requisition of a named GtkButton.
 * @param req    Out parameter: receives the measured requisition.
 * @param parent Container the temporary probe button is added to.
 * @param name   GTK widget name to assign the probe button (for theme/RC matching).
 */
void get_button_spacing(GtkRequisition *req, GtkContainer *parent, gchar *name);

/**
 * @brief Convert a GdkColor to a packed 24-bit integer.
 * @param color The color to convert.
 * @return Packed `0x00RRGGBB` value.
 */
guint32 gcolor2rgb24(GdkColor *color);

/**
 * @brief Convert a GdkColor to a "#RRGGBB" hex string.
 * @param color The color to convert.
 * @return Pointer to a static buffer containing the hex string.
 * @warning Not re-entrant -- the returned pointer refers to a static buffer
 *          that is overwritten by the next call.
 */
gchar *gdk_color_to_RRGGBB(GdkColor *color);

/* -----------------------------------------------------------------------
 * Pixbuf / image / button widget factory
 * ----------------------------------------------------------------------- */

/**
 * @brief Load a GdkPixbuf from an icon name or file path, with optional fallback.
 * @param iname        Icon-theme name to look up (or NULL).
 * @param fname        File path to load directly (or NULL).
 * @param width        Target width in pixels.
 * @param height       Target height in pixels.
 * @param use_fallback Whether to substitute a fallback image if lookup/load fails.
 * @return A new GdkPixbuf; caller owns the reference.
 */
GdkPixbuf *fb_pixbuf_new(gchar *iname, gchar *fname, int width, int height,
        gboolean use_fallback);

/**
 * @brief Create a GtkImage with automatic icon-theme-change tracking.
 * @param iname  Icon-theme name to look up (or NULL).
 * @param fname  File path to load directly (or NULL).
 * @param width  Target width in pixels.
 * @param height Target height in pixels.
 * @return A new, floating-reference GtkWidget (GtkImage).
 */
GtkWidget *fb_image_new(gchar *iname, gchar *fname, int width, int height);

/**
 * @brief Create a composite hover/press button widget (GtkBgBox + fb_image()).
 * @param iname   Icon-theme name to look up (or NULL).
 * @param fname   File path to load directly (or NULL).
 * @param width   Target width in pixels.
 * @param height  Target height in pixels.
 * @param hicolor Highlight tint colour applied on hover/press.
 * @param label   Unused -- accepted but never referenced in the
 *                implementation (verified against fbwidgets.c).
 * @return A new, floating-reference GtkWidget.
 */
GtkWidget *fb_button_new(gchar *iname, gchar *fname, int width, int height,
        gulong hicolor, gchar *label);

#endif /* FBWIDGETS_H */
