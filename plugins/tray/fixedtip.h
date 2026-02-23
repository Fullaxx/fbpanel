/* Fixed tooltip routine -- header.
 *
 * Copyright (C) 2001 Havoc Pennington, 2002 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Derived from Metacity's fixed-tip implementation.  Unlike the standard
 * GTK tooltip (which follows the mouse and uses screen-relative timers),
 * this tooltip window is positioned at an explicit root-window coordinate
 * "pointing" at a specific location adjacent to the panel edge.
 *
 * Public API:
 *   fixed_tip_show() -- show (or update) the tooltip window near the panel.
 *   fixed_tip_hide() -- destroy the tooltip window.
 *
 * The tooltip is a singleton popup window (static file-scope globals tip
 * and label); only one can be visible at a time.
 */

#ifndef FIXED_TIP_H
#define FIXED_TIP_H

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

/*
 * fixed_tip_show -- show a tooltip at a panel-relative position.
 *
 * Creates the tooltip window the first time; subsequent calls update
 * the position and markup text.
 *
 * Parameters:
 *   screen_number    - screen index (for multi-head support via
 *                      HAVE_GTK_MULTIHEAD; falls back to default screen).
 *   root_x, root_y  - root-window coordinate the tooltip should "point to"
 *                      (usually the tray icon position).
 *   strut_is_vertical - TRUE if the panel occupies a left/right strut;
 *                        FALSE for a top/bottom strut.
 *   strut            - the panel edge coordinate (e.g. panel height for a
 *                       bottom panel, panel width for a left panel).
 *   markup_text      - Pango markup string to display in the tooltip.
 *
 * Positioning logic:
 *   If strut_is_vertical: tip is placed to the right or left of the strut.
 *   If horizontal:        tip is placed above or below the strut.
 *   The tip is pushed on-screen if it would extend beyond screen bounds.
 */
void fixed_tip_show (int screen_number,
                     int root_x, int root_y,
                     gboolean strut_is_vertical,
                     int strut,
                     const char *markup_text);

/*
 * fixed_tip_hide -- destroy the tooltip window.
 *
 * Safe to call even if no tooltip is currently shown.
 * After this call the static "tip" pointer is NULL.
 */
void fixed_tip_hide (void);


#endif /* FIXED_TIP_H */
