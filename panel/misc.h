#ifndef MISC_H
#define MISC_H

/*
 * misc.h -- Public interface for fbpanel's miscellaneous utilities.
 *
 * Includes ewmh.h and fbwidgets.h so that any file including misc.h
 * continues to see all previously-public declarations without change.
 */

#include <gtk/gtk.h>
#include <stdio.h>

#include "panel.h"
#include "ewmh.h"
#include "fbwidgets.h"

/* Enum table lookup helpers */
int str2num(xconf_enum *p, gchar *str, int defval);
gchar *num2str(xconf_enum *p, int num, gchar *defval);

/* Panel initialisation and teardown */
void fb_init(void);
void fb_free(void);

/* EWMH desktop queries (declared in ewmh.h; exposed here for compat) */
//Window Select_Window(Display *dpy);

/* Panel geometry */
void calculate_position(panel *np);
gchar *expand_tilda(gchar *file);

/* Context menu positioning callback */
void menu_pos(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, GtkWidget *widget);

/* GTK config dialog launcher */
void configure();

/* Config file indentation helper */
gchar *indent(int level);

/* Profile file access */
FILE *get_profile_file(gchar *profile, char *perm);

#endif
