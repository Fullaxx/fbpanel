/*
 * fb-background-monitor.h:
 *
 * Copyright (C) 2001, 2002 Ian McKellar <yakk@yakk.net>
 *                     2002 Sun Microsystems, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *      Ian McKellar <yakk@yakk.net>
 *	Mark McLoughlin <mark@skynet.ie>
 */

/**
 * @file
 * @brief Public interface for FbBg, the root-window background monitor.
 *
 * FbBg is a GObject that tracks the X11 root pixmap (set by wallpaper
 * daemons via _XROOTPMAP_ID) and provides helpers for pseudo-transparency:
 * callers can request a copy of the background behind any widget or rect,
 * and optionally composite a tint colour over it.
 *
 * Typical usage:
 * @code
 *   FbBg *bg = fb_bg_get_for_display();
 *   g_signal_connect(bg, "changed", G_CALLBACK(my_handler), user_data);
 *   // ... later when repainting:
 *   GdkPixmap *pix = fb_bg_get_xroot_pix_for_win(bg, my_widget);
 *   // use pix, then:
 *   g_object_unref(pix);
 *   // ... at shutdown:
 *   g_object_unref(bg);
 * @endcode
 *
 * @note Vendored from "fb-background-monitor" (Ian McKellar, Mark
 *       McLoughlin; 2001-2002), licensed LGPL-2.0-or-later. See
 *       docs/THIRD_PARTY_NOTICES.md.
 * @bug This needs to be made multiscreen aware: panel_bg_get should take
 *      a GdkScreen argument.
 */

#ifndef __FB_BG_H__
#define __FB_BG_H__

#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <X11/Xlib.h>

/**
 * @name GObject type macros for FbBg.
 * @{
 * @def FB_TYPE_BG
 *   The GType for FbBg; use in g_object_new() etc.
 * @def FB_BG(o)
 *   Cast a GObject pointer to FbBg* (with type check in debug).
 * @def FB_BG_CLASS(k)
 *   Cast a GObjectClass pointer to FbBgClass*.
 * @def FB_IS_BG(o)
 *   Runtime type check; TRUE if @p o is an FbBg.
 * @def FB_IS_BG_CLASS(k)
 *   Runtime type check on the class pointer.
 * @def FB_BG_GET_CLASS(o)
 *   Retrieve the FbBgClass* from an instance pointer.
 * @}
 */
#define FB_TYPE_BG         (fb_bg_get_type ())
#define FB_BG(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o),      \
				       FB_TYPE_BG,        \
				       FbBg))
#define FB_BG_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k),         \
				       FB_TYPE_BG,        \
				       FbBgClass))
#define FB_IS_BG(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o),      \
				       FB_TYPE_BG))
#define FB_IS_BG_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k),         \
				       FB_TYPE_BG))
#define FB_BG_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o),       \
				       FB_TYPE_BG,        \
				       FbBgClass))

/*
 * Opaque type declarations.
 * The struct definitions live in bg.c and are private.
 * External code must use the public API functions below.
 */
typedef struct _FbBgClass FbBgClass;
typedef struct _FbBg      FbBg;

/**
 * @brief Return the GType for the FbBg class.
 *
 * Registers the type on first call. Used by the #FB_TYPE_BG and #FB_BG()
 * macros; should not normally be called directly.
 */
GType fb_bg_get_type       (void);

/**
 * @brief Create a fresh, standalone FbBg instance.
 *
 * In most cases prefer fb_bg_get_for_display() to share a single instance
 * across the application.
 *
 * @return A new FbBg with ref count 1. Caller must g_object_unref().
 */
FbBg *fb_bg_new(void);

/**
 * @brief Tint a GdkDrawable in-place by compositing a colour over it.
 *
 * @param base      The GdkDrawable (pixmap) to modify in-place.
 * @param gc        A GdkGC used to render the result back onto @p base.
 * @param tintcolor 32-bit tint colour (used for both checkerboard colours
 *                  in gdk_pixbuf_composite_color_simple).
 * @param alpha     Background opacity [0..255]; 0 = fully tinted, 255 = no tint.
 * @note @p base is modified in-place; no caller-visible allocations are made.
 */
void fb_bg_composite(GdkDrawable *base, GdkGC *gc, guint32 tintcolor, gint alpha);

/**
 * @brief Capture the root background behind a widget.
 *
 * @param bg     A valid FbBg instance (must have a non-None root pixmap).
 * @param widget A realised GtkWidget whose screen position is used.
 *               `widget->window` must be non-NULL (widget must be realised).
 * @return A new GdkPixmap (ref count 1) containing the background, or NULL
 *         if @p bg has no root pixmap, or if the widget geometry is
 *         unavailable. Caller owns the result and must call
 *         g_object_unref() on it.
 */
GdkPixmap *fb_bg_get_xroot_pix_for_win(FbBg *bg, GtkWidget *widget);

/**
 * @brief Capture the root background for a screen rectangle.
 *
 * @param bg     A valid FbBg instance (must have a non-None root pixmap).
 * @param x      Left edge of the rectangle in root-window (screen) coordinates.
 * @param y      Top edge of the rectangle in root-window (screen) coordinates.
 * @param width  Width of the rectangle in pixels.
 * @param height Height of the rectangle in pixels.
 * @param depth  Colour depth of the new pixmap (should match screen depth).
 * @return A new GdkPixmap (ref count 1) containing the background, or NULL
 *         if @p bg has no root pixmap or allocation fails. Caller owns the
 *         result and must call g_object_unref() on it.
 */
GdkPixmap *fb_bg_get_xroot_pix_for_area(FbBg *bg,gint x, gint y,
      gint width, gint height, gint depth);

/**
 * @brief Return the cached X11 Pixmap ID for the root background.
 *
 * @param bg A valid FbBg instance.
 * @return The X11 Pixmap ID, or None (0) if no root pixmap is set. The
 *         returned ID is owned by the X server / wallpaper daemon; caller
 *         must NOT call XFreePixmap() on it.
 */
Pixmap fb_bg_get_xrootpmap(FbBg *bg);

/**
 * @brief Signal that the root background pixmap has changed.
 *
 * Emits the "changed" GObject signal on @p bg, which:
 *   -# Re-reads _XROOTPMAP_ID and updates the internal GC tile.
 *   -# Calls all user callbacks connected with g_signal_connect().
 *
 * Typically called from the panel's X PropertyNotify event handler when
 * it detects a change to the _XROOTPMAP_ID root window property.
 *
 * @param bg The FbBg instance to notify.
 */
void fb_bg_notify_changed_bg(FbBg *bg);

/**
 * @brief Return the process-wide shared FbBg singleton.
 *
 * On the first call, a new FbBg is created (ref count = 1). On subsequent
 * calls, g_object_ref() is called on the existing instance (ref count is
 * incremented). When the last reference is dropped and fb_bg_finalize
 * runs, the singleton pointer is cleared so a fresh instance can be
 * created on the next call.
 *
 * @return The shared FbBg instance with its reference count incremented.
 *         Caller must call g_object_unref() when done.
 */
FbBg *fb_bg_get_for_display(void);

#endif /* __FB_BG_H__ */
