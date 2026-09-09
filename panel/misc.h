#ifndef MISC_H
#define MISC_H

/**
 * @file
 * @brief Public interface for fbpanel's miscellaneous utilities.
 *
 * Includes ewmh.h and fbwidgets.h so that any file including misc.h
 * continues to see all previously-public declarations without change.
 */

#include <gtk/gtk.h>
#include <stdio.h>

#include "panel.h"
#include "ewmh.h"
#include "fbwidgets.h"

/**
 * @brief Look up a string in an xconf_enum table and return its int value.
 * @param p      NULL-terminated xconf_enum table to search.
 * @param str    String key to look up.
 * @param defval Value returned if @p str is not found in the table.
 * @return The matching int value, or @p defval if not found.
 */
int str2num(xconf_enum *p, gchar *str, int defval);

/**
 * @brief Look up an int in an xconf_enum table and return its string key.
 * @param p      NULL-terminated xconf_enum table to search.
 * @param num    Int value to look up.
 * @param defval String returned if @p num is not found in the table.
 * @return The matching string key, or @p defval if not found.
 */
gchar *num2str(xconf_enum *p, int num, gchar *defval);

/**
 * @brief Initialise process-wide panel state.
 *
 * Interns all X11/EWMH atoms (resolve_atoms()), and initialises the
 * global icon theme and the ::fbev EWMH event bus.
 */
void fb_init(void);

/**
 * @brief Tear down process-wide panel state allocated by fb_init().
 */
void fb_free(void);

/* EWMH desktop queries (declared in ewmh.h; exposed here for compat) */
//Window Select_Window(Display *dpy);

/**
 * @brief Compute the panel's on-screen position and size.
 *
 * Xinerama/multi-monitor-aware: uses `np->screenRect` (the target
 * monitor's geometry) together with edge/align/margin/width/height
 * config to fill in `np->ax/ay/aw/ah`.
 *
 * @param np The panel to position.
 */
void calculate_position(panel *np);

/**
 * @brief Expand a leading `~` in a path to the user's home directory.
 * @param file Path, optionally starting with `~`.
 * @return Newly allocated expanded path; caller must g_free().
 */
gchar *expand_tilda(gchar *file);

/**
 * @brief GtkMenuPositionFunc callback for positioning the panel's context menu.
 *
 * Matches the standard GTK2 `GtkMenuPositionFunc` signature (with
 * `widget` in place of a generic `user_data`).
 *
 * @param menu    The menu being positioned.
 * @param x       Out parameter: x coordinate to place the menu at.
 * @param y       Out parameter: y coordinate to place the menu at.
 * @param push_in Out parameter: whether GTK should push the menu on-screen.
 * @param widget  The widget the menu was popped up from.
 */
void menu_pos(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, GtkWidget *widget);

/**
 * @brief Open the "Configure Panel" preferences dialog.
 * @param xc Root of the config tree to edit (see gconf.c/gconf_panel.c/gconf_plugins.c).
 */
void configure(xconf *xc);

/**
 * @brief Build an indentation string for config-file serialisation.
 * @param level Nesting depth.
 * @return Newly allocated string of whitespace for the given nesting level.
 */
gchar *indent(int level);

/**
 * @brief Open a profile's config file.
 * @param profile Profile name (e.g. "default").
 * @param perm    fopen()-style mode string (e.g. "r", "w").
 * @return An open FILE*, or NULL on failure. Caller must fclose() it.
 */
FILE *get_profile_file(gchar *profile, char *perm);

#endif
