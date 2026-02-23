/* genmon -- Generic Monitor plugin for fbpanel.
 *
 * Copyright (C) 2007 Davide Truffa <davide@catoblepa.org>
 *
 * This plugin is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 dated June, 1991.
 *
 * It is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * genmon periodically runs an external shell command via popen(), reads the
 * first line of its output, and displays it in a Pango-markup GtkLabel
 * wrapped in a configurable span tag (size and colour).
 *
 * Timer: gm->timer is a g_timeout_add handle firing every gm->time seconds.
 * Memory: gm->command, gm->textsize, gm->textcolor point into xconf storage
 *         or literal strings -- do not free. gm->main is owned by GTK.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUG
#include "dbg.h"

/*
 * FMT -- Pango markup span template.
 * Arguments: size string (e.g. "medium"), foreground colour string (e.g.
 * "darkblue"), and the escaped command output text.
 * g_markup_printf_escaped is used so the text itself is safe inside XML.
 */
#define FMT "<span size='%s' foreground='%s'>%s</span>"

/*
 * genmon_priv -- per-instance state.
 *
 * Memory ownership:
 *   command    -- points into xconf storage or the literal "date +%R"; do not free.
 *   textsize   -- xconf storage or literal "medium"; do not free.
 *   textcolor  -- xconf storage or literal "darkblue"; do not free.
 *   main       -- GtkLabel; owned by GTK widget tree.
 *   timer      -- g_timeout_add GSource handle; 0 when not running.
 */
typedef struct {
    plugin_instance plugin; /* base class -- must be first */
    int time;               /* polling interval in seconds (default 1)       */
    int timer;              /* g_timeout_add handle; 0 = not scheduled       */
    int max_text_len;       /* maximum characters displayed in the label     */
    char *command;          /* shell command to popen()                      */
    char *textsize;         /* Pango size string, e.g. "medium", "small"    */
    char *textcolor;        /* Pango colour string, e.g. "darkblue", "#f00" */
    GtkWidget *main;        /* GtkLabel displaying command output            */
} genmon_priv;

/*
 * text_update -- timer callback; runs the command and updates the label.
 *
 * Parameters:
 *   gm -- genmon_priv instance (cast from GSourceFunc gpointer).
 *
 * Returns: TRUE (keep the timer running).
 *
 * Algorithm:
 *   1. popen() the configured command in read mode.
 *   2. Read one line (up to 255 chars) with fgets().
 *   3. Strip the trailing newline if present.
 *   4. Build a Pango markup string using g_markup_printf_escaped (which
 *      escapes '<', '>', '&' in the text so it is safe in XML context).
 *   5. Pass the markup string to gtk_label_set_markup.
 *   6. Free the markup string.
 *
 * BUG: popen() return value is not checked for NULL (command not found,
 *      out of file descriptors). fgets() is called on a potentially NULL
 *      FILE* which would cause a NULL-pointer dereference crash.
 *
 * BUG: pclose() return value (exit code of command) is silently discarded.
 *
 * BUG: If fgets() returns NULL (command produced no output, or read error),
 *      strlen(text) is called on an uninitialised or empty buffer; len may
 *      be -1 if text[0]=='\0' only when len is cast/computed as int from 0-1
 *      -- actually strlen returns 0, so len = -1, the if(len>=0) branch is
 *      skipped, which is safe, but the empty output case is silently ignored.
 */
static int
text_update(genmon_priv *gm)
{
    FILE *fp;
    char text[256]; // output buffer; reads at most 255 chars + NUL
    char *markup;
    int len;

    ENTER;
    // Run the configured command; fp is NULL on error (not checked -- bug)
    fp = popen(gm->command, "r");
    // Read the first line of command output; text may be uninitialised if fp is NULL
    (void)fgets(text, sizeof(text), fp);
    pclose(fp); // reap the child process; exit code discarded
    len = strlen(text) - 1; // index of last character (or -1 if empty)
    if (len >= 0) {
        // Strip trailing newline from shell command output
        if (text[len] == '\n')
            text[len] = 0;

        // Wrap the text in a Pango markup span with configured size and colour.
        // g_markup_printf_escaped escapes XML-special chars in 'text' argument.
        markup = g_markup_printf_escaped(FMT, gm->textsize, gm->textcolor,
            text);
        gtk_label_set_markup (GTK_LABEL(gm->main), markup);
        g_free(markup); // free the dynamically allocated markup string
    }
    RET(TRUE); // returning TRUE keeps the periodic timer alive
}

/*
 * genmon_destructor -- clean up on plugin unload.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 *
 * Removes the periodic timer. The GtkLabel (gm->main) is destroyed
 * as part of the p->pwid widget tree by the framework.
 */
static void
genmon_destructor(plugin_instance *p)
{
    genmon_priv *gm = (genmon_priv *) p;

    ENTER;
    if (gm->timer) {
        g_source_remove(gm->timer); // stop periodic popen() calls
    }
    RET();
}

/*
 * genmon_constructor -- initialise the generic monitor plugin.
 *
 * Parameters:
 *   p -- plugin_instance allocated by the framework.
 *
 * Returns: 1 on success.
 *
 * Configuration keys (via XCG):
 *   Command       -- shell command to run (default "date +%R")
 *   TextSize      -- Pango size token (default "medium")
 *   TextColor     -- Pango colour string (default "darkblue")
 *   PollingTime   -- interval in seconds (default 1)
 *   MaxTextLength -- max chars in label (default 30)
 *
 * Widget: gm->main is a GtkLabel added to p->pwid.
 * Timer: gm->timer = g_timeout_add(gm->time * 1000, text_update, gm).
 *        Must be cancelled in genmon_destructor.
 *
 * BUG: gm->time is stored as int (seconds) but multiplied by 1000 when passed
 *      to g_timeout_add. If gm->time > 2147 (seconds), the multiplication
 *      overflows a 32-bit int, creating a very short or near-zero interval.
 *
 * BUG: if gm->time == 0 (user sets PollingTime=0), g_timeout_add(0, ...) is
 *      called, which fires the callback on every GTK main loop iteration,
 *      effectively making popen() spin at 100% CPU.
 */
static int
genmon_constructor(plugin_instance *p)
{
    genmon_priv *gm;

    ENTER;
    gm = (genmon_priv *) p;
    // Set defaults before reading user configuration
    gm->command = "date +%R";   // default: display the current time
    gm->time = 1;               // default: update every 1 second
    gm->textsize = "medium";    // default Pango size token
    gm->textcolor = "darkblue"; // default Pango colour
    gm->max_text_len = 30;      // default max characters in label

    // Read per-instance configuration overrides from xconf
    XCG(p->xc, "Command", &gm->command, str);
    XCG(p->xc, "TextSize", &gm->textsize, str);
    XCG(p->xc, "TextColor", &gm->textcolor, str);
    XCG(p->xc, "PollingTime", &gm->time, int);
    XCG(p->xc, "MaxTextLength", &gm->max_text_len, int);

    // Create the GtkLabel with a character width limit to cap the plugin width
    gm->main = gtk_label_new(NULL);
    gtk_label_set_max_width_chars(GTK_LABEL(gm->main), gm->max_text_len);
    text_update(gm); // run command immediately to show initial value
    gtk_container_set_border_width (GTK_CONTAINER (p->pwid), 1);
    gtk_container_add(GTK_CONTAINER(p->pwid), gm->main);
    gtk_widget_show_all(p->pwid);
    // Schedule the recurring timer; interval is gm->time seconds
    gm->timer = g_timeout_add((guint) gm->time * 1000,
        (GSourceFunc) text_update, (gpointer) gm);

    RET(1);
}


/* Plugin class descriptor */
static plugin_class class = {
    .count       = 0,
    .type        = "genmon",
    .name        = "Generic Monitor",
    .version     = "0.3",
    .description = "Display the output of a program/script into the panel",
    .priv_size   = sizeof(genmon_priv),

    .constructor = genmon_constructor,
    .destructor  = genmon_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
