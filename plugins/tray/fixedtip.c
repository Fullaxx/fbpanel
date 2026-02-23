/* Metacity fixed tooltip routine */

/*
 * Copyright (C) 2001 Havoc Pennington
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
 */

/*
 * fixedtip.c -- singleton fixed tooltip window positioned adjacent to the panel.
 *
 * Unlike standard GTK tooltips (which follow the mouse and use
 * screen-relative timers), this tooltip is placed at an explicit
 * root-window coordinate next to the panel strut edge.
 *
 * Derived from Metacity's fixed-tip implementation.
 *
 * Singleton state:
 *   tip          - the popup GtkWindow (NULL when not visible)
 *   label        - the GtkLabel child of tip
 *   screen_width / screen_height - cached screen dimensions (for push-onscreen)
 *
 * Public API:
 *   fixed_tip_show() - create or update the tooltip window
 *   fixed_tip_hide() - destroy the tooltip window
 */

#include "fixedtip.h"

/* Singleton tooltip window.  NULL when no tooltip is currently visible.
 * The "destroy" signal connects gtk_widget_destroyed(&tip) so this is
 * also zeroed if the window is destroyed externally.                     */
static GtkWidget *tip = NULL;

/* Label child of tip; carries the Pango markup text.                     */
static GtkWidget *label = NULL;

/* Screen dimensions cached at tip-creation time for push-onscreen math.  */
static int screen_width = 0;
static int screen_height = 0;

/*
 * button_press_handler -- dismiss the tooltip on any button press.
 *
 * Connected to the "button_press_event" signal of the tip window.
 * Returns FALSE so the event continues to propagate normally.
 */
static gboolean
button_press_handler (GtkWidget *tip,
                      GdkEvent  *event,
                      void      *data)
{
  fixed_tip_hide ();

  return FALSE;
}

/*
 * expose_handler -- paint the tooltip background using the tooltip theme.
 *
 * Connected to the "expose_event" signal of the tip window.
 *
 * BUG: the function signature declares `GtkTooltips *tooltips` as the
 *      first parameter, but GTK passes `(GtkWidget *, GdkEventExpose *,
 *      gpointer)` for "expose_event".  The first argument is actually
 *      a GtkWidget* (the tip window itself), not a GtkTooltips*.  This
 *      compiles silently but is a type mismatch; the body accesses
 *      tip->style and tip->window via the file-scope static rather than
 *      the callback argument, so the paint call is functionally correct,
 *      but the signature is still wrong (undefined behaviour).
 */
static gboolean
expose_handler (GtkTooltips *tooltips)
{
  /* Paint the tooltip box chrome using the GTK "tooltip" detail string.
   * Uses the file-scope static `tip` rather than the callback argument
   * because the parameter type is wrong (see BUG note above).           */
  gtk_paint_flat_box (tip->style, tip->window,
                      GTK_STATE_NORMAL, GTK_SHADOW_OUT,
                      NULL, tip, "tooltip",
                      0, 0, -1, -1);

  return FALSE;
}

/*
 * fixed_tip_show -- create or update the singleton tooltip window.
 *
 * On the first call a GTK_WINDOW_POPUP is created, styled as a tooltip,
 * and the screen dimensions are cached.  Subsequent calls just update
 * the markup text and reposition the window.
 *
 * Parameters:
 *   screen_number     - X screen index (used only under HAVE_GTK_MULTIHEAD)
 *   root_x, root_y   - root-window coordinate the tip should point at
 *   strut_is_vertical - TRUE if panel has a left/right strut
 *   strut             - panel edge coordinate (height for top/bottom panel,
 *                       width for left/right panel)
 *   markup_text       - Pango markup string for the tip label
 *
 * Positioning logic (after measuring the tip window's natural size):
 *   Vertical panel:   move x to right/left of strut; center y on root_y
 *   Horizontal panel: move y to above/below strut;   center x on root_x
 *   Then push on-screen if the window would extend past the right/bottom edge.
 */
void
fixed_tip_show (int screen_number,
                int root_x, int root_y,
                gboolean strut_is_vertical,
                int strut,
                const char *markup_text)
{
  int w, h;

  if (tip == NULL)
    {
      /* --- First call: create the tooltip window --- */
      tip = gtk_window_new (GTK_WINDOW_POPUP);

#ifdef HAVE_GTK_MULTIHEAD
      {
        /* Multi-head support: place the tip on the correct X screen and
         * cache that screen's dimensions.                                 */
        GdkScreen *gdk_screen;

        gdk_screen = gdk_display_get_screen (gdk_get_default_display (),
                                             screen_number);
        gtk_window_set_screen (GTK_WINDOW (tip),
                               gdk_screen);
        screen_width = gdk_screen_get_width (gdk_screen);
        screen_height = gdk_screen_get_height (gdk_screen);
      }
#else
      /* Single-head fallback: use the default screen dimensions.          */
      screen_width = gdk_screen_width ();
      screen_height = gdk_screen_height ();
#endif

      /* Make the window app-paintable so expose_handler can draw freely.  */
      gtk_widget_set_app_paintable (tip, TRUE);
      /* gtk_window_set_policy was deprecated; use set_resizable instead.  */
      //gtk_window_set_policy (GTK_WINDOW (tip), FALSE, FALSE, TRUE);
      gtk_window_set_resizable(GTK_WINDOW (tip), FALSE);

      /* Name the window "gtk-tooltips" so it inherits the tooltip theme.  */
      gtk_widget_set_name (tip, "gtk-tooltips");

      /* 4-pixel border inside the popup frame.                            */
      gtk_container_set_border_width (GTK_CONTAINER (tip), 4);

      /* Paint background chrome via expose_handler (see BUG note there).  */
      g_signal_connect (G_OBJECT (tip),
            "expose_event",
            G_CALLBACK (expose_handler),
            NULL);

      /* Allow button-press events so we can dismiss the tip on click.     */
      gtk_widget_add_events (tip, GDK_BUTTON_PRESS_MASK);

      g_signal_connect (G_OBJECT (tip),
            "button_press_event",
            G_CALLBACK (button_press_handler),
            NULL);

      /* Create the label widget.  Markup is set in every call below.      */
      label = gtk_label_new (NULL);
      gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
      gtk_misc_set_alignment (GTK_MISC (label), 0.5, 0.5);
      gtk_widget_show (label);

      gtk_container_add (GTK_CONTAINER (tip), label);

      /* When the window is destroyed externally (e.g. if another piece of
       * code calls gtk_widget_destroy on tip), zero the static pointer so
       * the next call to fixed_tip_show() recreates it correctly.         */
      g_signal_connect (G_OBJECT (tip),
            "destroy",
            G_CALLBACK (gtk_widget_destroyed),
            &tip);
    }

  /* Update the label text (Pango markup).                                 */
  gtk_label_set_markup (GTK_LABEL (label), markup_text);

  /* FIXME should also handle Xinerama here, just to be
   * really cool
   */

  /* Measure the tip's natural size.  Note: the window may not yet be
   * realized on the first call, so w and h might both be 0.              */
  gtk_window_get_size (GTK_WINDOW (tip), &w, &h);

  /* Pixels of padding between the panel edge and the tip window.         */
#define PAD 5

  if (strut_is_vertical)
    {
      /* Left/right panel: place the tip to the right of the strut if the
       * strut edge (x coordinate) is > root_x (i.e. panel is on the
       * left), otherwise place it to the left.
       * Center the tip vertically around root_y.                          */
      if (strut > root_x)
        root_x = strut + PAD;
      else
        root_x = strut - w - PAD;

      root_y -= h / 2;
    }
  else
    {
      /* Top/bottom panel: place the tip below the strut if the strut
       * edge (y coordinate) is > root_y (panel at top), otherwise above.
       * Center the tip horizontally around root_x.                        */
      if (strut > root_y)
        root_y = strut + PAD;
      else
        root_y = strut - h - PAD;

      root_x -= w / 2;
    }

  /* Push on-screen: if the window extends past the right or bottom edge,
   * pull it back just enough to stay on screen.                           */
  if ((root_x + w) > screen_width)
    root_x -= (root_x + w) - screen_width;

  if ((root_y + h) > screen_height)
    root_y -= (root_y + h) - screen_height;

  gtk_window_move (GTK_WINDOW (tip), root_x, root_y);

  /* Show the tooltip window (no-op if already visible).                   */
  gtk_widget_show (tip);
}

/*
 * fixed_tip_hide -- destroy the singleton tooltip window.
 *
 * Safe to call when no tooltip is visible (tip == NULL).
 * After this call tip is NULL (also zeroed by the gtk_widget_destroyed
 * "destroy" callback registered in fixed_tip_show, so the explicit
 * `tip = NULL` below is redundant but harmless).
 */
void
fixed_tip_hide (void)
{
  if (tip)
    {
      gtk_widget_destroy (tip);
      tip = NULL;  /* redundant: gtk_widget_destroyed already zeroes this */
    }
}
