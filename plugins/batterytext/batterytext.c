/* batterytext_priv.c -- Generic monitor plugin for fbpanel
 *
 * Copyright (C) 2017 Fred Stober <mail@fredstober.de>
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
 * Overview:
 *   Displays battery charge as a text label using raw sysfs files rather
 *   than the power_supply abstraction layer.  Reads individual files such
 *   as "energy_full_design", "energy_full", "energy_now", "power_now" and
 *   "status" directly from the battery path (default: BAT0).
 *
 * Plugin lifecycle:
 *   batterytext_constructor() -- creates GtkLabel, starts polling timer.
 *   text_update()             -- called every `time` ms; refreshes label+tooltip.
 *   batterytext_destructor()  -- cancels timer.
 *
 * Timer management:
 *   gm->timer stores the GSource ID returned by g_timeout_add().
 *   It MUST be removed in batterytext_destructor() via g_source_remove().
 *   The destructor checks (gm->timer != 0) before removal as a safety guard.
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
 * batterytext_priv -- per-instance private data for the batterytext plugin.
 *
 * Layout note: plugin_instance MUST be the first field so the struct can
 * be safely cast to plugin_instance* by the fbpanel framework.
 *
 * Timer: gm->timer holds the g_timeout_add() source ID.
 *   Must be removed in batterytext_destructor() via g_source_remove().
 *
 * String fields (textsize, battery): These are set either to string
 *   literals (the defaults) or to strings owned by the XCG config system.
 *   They must NOT be freed by this plugin.
 */
typedef struct {
    plugin_instance plugin;  // MUST be first: framework casts p -> plugin_instance*
    int design;              // if non-zero, use energy_full_design instead of energy_full
    int time;                // polling interval in milliseconds (default: 500)
    char *textsize;          // Pango size string (e.g. "medium", "large"); NOT owned by plugin
    char *battery;           // path to battery sysfs directory (e.g. "/sys/class/power_supply/BAT0")
                             // NOT owned by plugin (points to literal or XCG-managed string)
    int timer;               // GLib timer source ID (from g_timeout_add()); 0 if not running
    GtkWidget *main;         // GtkLabel widget displaying the charge percentage; owned by GTK
} batterytext_priv;

/*
 * read_bat_value -- read a single float value from a sysfs battery attribute file.
 *
 * Constructs the path "<dn>/<fn>", opens the file, reads one float,
 * then closes the file.  Returns -1 if the file cannot be opened or
 * the value cannot be parsed.
 *
 * Parameters:
 *   dn -- directory path (e.g. "/sys/class/power_supply/BAT0").
 *         Must not be NULL.
 *   fn -- file name within the directory (e.g. "energy_now").
 *         Must not be NULL.
 *
 * Returns: The parsed float value on success, or -1.0f on failure
 *   (file not found, permission denied, or non-numeric content).
 *
 * Memory: value_path is stack-allocated (256 bytes).  No heap allocations.
 *
 * File descriptors: The FILE* is always closed before returning, even on
 *   parse failure.  No file descriptor leaks.
 *
 * WARNING: value_path is 256 bytes. If strlen(dn) + strlen(fn) + 2
 *   exceeds 255, g_snprintf will silently truncate, potentially opening
 *   the wrong file.  See BUGS section.
 */
static float
read_bat_value(const char *dn, const char *fn)
{
    FILE *fp;
    float value = -1;         // sentinel: -1 signals "unreadable"
    char value_path[256];     // stack-allocated path buffer (256 bytes max)

    // Build the full path: "<dn>/<fn>" (g_snprintf null-terminates and truncates safely).
    g_snprintf(value_path, sizeof(value_path), "%s/%s", dn, fn);

    fp = fopen(value_path, "r");
    if (fp != NULL) {
        // Attempt to parse exactly one float from the file.
        if (fscanf(fp, "%f", &value) != 1)
            value = -1; // parse failed (e.g. empty file or unexpected format)
        fclose(fp);     // always close; prevents file descriptor leak
    }
    return value;
}

/*
 * text_update -- periodic GSourceFunc: refresh the battery label and tooltip.
 *
 * Reads energy/power values from sysfs, computes the charge ratio and
 * estimated time remaining (or time to full charge), formats a Pango
 * markup label (red for discharging, green for charging), and updates
 * the GTK label and tooltip.
 *
 * Parameters:
 *   gm -- batterytext_priv* for this plugin instance.  Must not be NULL.
 *         Cast to gpointer by the GLib timer infrastructure.
 *
 * Returns: TRUE to keep the timer firing (GSourceFunc contract).
 *   Returning FALSE would cancel the timer and leave gm->timer as a
 *   stale/invalid source ID, which could then be incorrectly passed to
 *   g_source_remove() in the destructor.
 *
 * Memory:
 *   markup and tooltip are allocated by g_markup_printf_escaped() and
 *   must be freed with g_free() -- done in this function before returning.
 *
 * File descriptors: fp_status is always closed after reading.
 *
 * WARNING (medium): Division by zero possible when computing charge_time
 *   if power_now == 0 (battery is full or no power draw measured).
 *   Both "(energy_now / power_now)" and "((energy_full - energy_now) / power_now)"
 *   will produce +Inf when power_now is zero; cast to int gives undefined
 *   behaviour in C.  See BUGS section.
 *
 * WARNING (medium): If energy_full is zero (corrupt/unusual firmware),
 *   "100 * energy_now / energy_full" also divides by zero.
 *
 * WARNING (low): battery_status path buffer is 256 bytes; same truncation
 *   risk as value_path in read_bat_value().
 */
static int
text_update(batterytext_priv *gm)
{
    FILE *fp_status;            // FILE* for the "status" file
    char battery_status[256];   // path buffer for "<gm->battery>/status"
    char *markup;               // Pango markup string (heap; freed before return)
    char *tooltip;              // tooltip markup string (heap; freed before return)
    char buffer[256];           // line buffer for fgets() when reading status file
    float energy_full_design = -1; // from energy_full_design sysfs file (uWh)
    float energy_full        = -1; // from energy_full sysfs file (uWh)
    float energy_now         = -1; // from energy_now sysfs file (uWh)
    float power_now          = -1; // from power_now sysfs file (uW)
    int   discharging        = 0;  // 1 if the status file contains "Discharging"
    float charge_ratio       = 0;  // percentage of design or full capacity (0..100)
    int   charge_time        = 0;  // estimated seconds until empty or full

    ENTER;

    // Read raw sysfs attribute files for this battery.
    energy_full_design = read_bat_value(gm->battery, "energy_full_design");
    energy_full        = read_bat_value(gm->battery, "energy_full");
    energy_now         = read_bat_value(gm->battery, "energy_now");
    power_now          = read_bat_value(gm->battery, "power_now");

    // Build path to the status file and read it line by line.
    snprintf(battery_status, sizeof(battery_status), "%s/status", gm->battery);
    fp_status = fopen(battery_status, "r");
    if (fp_status != NULL) {
        while ((fgets(buffer, sizeof(buffer), fp_status)) != NULL) {
            // Check each line for the "Discharging" token.
            if (strstr(buffer, "Discharging") != NULL)
                discharging = 1;
        }
        fclose(fp_status); // always close; prevents fd leak
    }

    // Only compute display values if we have valid energy readings.
    // Both energy_full_design and energy_now must be >= 0 (not the -1 sentinel).
    if ((energy_full_design >= 0) && (energy_now >= 0)) {
        // Compute charge ratio: percentage of current energy vs. reference capacity.
        if (gm->design)
            charge_ratio = 100 * energy_now / energy_full_design; // vs. factory design capacity
        else
            charge_ratio = 100 * energy_now / energy_full;         // vs. current measured capacity
                                                                    // BUG: energy_full could be 0

        if (discharging)
        {
            // Red label with '-' suffix indicating discharging.
            markup = g_markup_printf_escaped("<span size='%s' foreground='red'><b>%.2f-</b></span>",
                gm->textsize, charge_ratio);
            // Estimate seconds remaining: energy_now / power_now * 3600
            // BUG: power_now == 0 causes integer division by zero via float -> int cast.
            charge_time = (int)(energy_now / power_now * 3600);
        }
        else
        {
            // Green label with '+' suffix indicating charging (or full).
            markup = g_markup_printf_escaped("<span size='%s' foreground='green'><b>%.2f+</b></span>",
                gm->textsize, charge_ratio);
            // Estimate seconds to full: (energy_full - energy_now) / power_now * 3600
            // BUG: power_now == 0 causes the same int-truncation of +Inf.
            charge_time = (int)((energy_full - energy_now) / power_now * 3600);
        }

        // Format tooltip as HH:MM:SS time estimate.
        tooltip = g_markup_printf_escaped("%02d:%02d:%02d",
            charge_time / 3600, (charge_time / 60) % 60, charge_time % 60);

        // Update the GTK label text and tooltip.
        gtk_label_set_markup (GTK_LABEL(gm->main), markup);
        g_free(markup);  // markup is heap-allocated by g_markup_printf_escaped
        gtk_widget_set_tooltip_markup (gm->main, tooltip);
        g_free(tooltip); // tooltip is heap-allocated by g_markup_printf_escaped
    }
    else
    {
        // No valid readings: show "N/A" for both label and tooltip.
        gtk_label_set_markup (GTK_LABEL(gm->main), "N/A");
        gtk_widget_set_tooltip_markup (gm->main, "N/A");
    }
    RET(TRUE); // keep timer alive
}

/*
 * batterytext_destructor -- fbpanel plugin destructor.
 *
 * Cancels the periodic polling timer.  The GtkLabel (gm->main) is a
 * child of p->pwid and is destroyed automatically by the GTK widget
 * hierarchy teardown managed by the fbpanel framework.
 *
 * Parameters:
 *   p -- plugin_instance* (same pointer passed to batterytext_constructor).
 *        Must not be NULL.
 *
 * Timer cleanup: g_source_remove(gm->timer) cancels the GLib timer.
 *   The guard (gm->timer != 0) prevents passing 0 to g_source_remove(),
 *   which would print a GLib warning.
 *
 * Signal cleanup: No g_signal_connect() calls are made in this plugin,
 *   so no signal handlers need to be disconnected.
 *
 * NOTE: gm->textsize and gm->battery are NOT freed here because they
 *   point either to string literals or to strings managed by the XCG
 *   config system, not heap memory owned by this plugin.
 */
static void
batterytext_destructor(plugin_instance *p)
{
    batterytext_priv *gm = (batterytext_priv *) p;

    ENTER;
    if (gm->timer) {
        g_source_remove(gm->timer); // cancel the periodic polling timer
    }
    RET();
}

/*
 * batterytext_constructor -- fbpanel plugin constructor.
 *
 * Initialises the plugin by:
 *   1. Setting default configuration values.
 *   2. Reading overrides from the config system via XCG().
 *   3. Creating the GtkLabel widget and performing an immediate update.
 *   4. Registering a periodic polling timer.
 *
 * Parameters:
 *   p -- plugin_instance* allocated by the fbpanel framework.
 *        sizeof(*p) == priv_size == sizeof(batterytext_priv).
 *        Must not be NULL.
 *
 * Returns: 1 on success, 0 on failure (currently never fails after widget
 *   creation because no g_source_add failure is checked).
 *
 * Timer: gm->timer receives the GSource ID from g_timeout_add().
 *   MUST be cancelled in batterytext_destructor().
 *
 * Memory:
 *   gm->main (GtkLabel) is owned by the GTK widget hierarchy; destroyed
 *   when the parent container is destroyed.
 *   gm->textsize and gm->battery point to literals or XCG-managed strings;
 *   do NOT free them in the destructor.
 *
 * NOTE: gm->time is the polling interval in milliseconds.  The default
 *   of 500 ms is quite frequent for a battery plugin (typical battery
 *   state only changes over seconds or minutes), but is configurable.
 */
static int
batterytext_constructor(plugin_instance *p)
{
    batterytext_priv *gm;

    ENTER;
    gm = (batterytext_priv *) p; // safe: batterytext_priv begins with plugin_instance

    // Set default configuration values before potentially overriding via config.
    gm->design   = False;                                // use energy_full, not design capacity
    gm->time     = 500;                                  // poll every 500 ms by default
    gm->textsize = "medium";                             // Pango font size token
    gm->battery  = "/sys/class/power_supply/BAT0";       // default battery sysfs path

    // Read per-instance configuration from the fbpanel config file.
    // XCG() overwrites the above defaults only if the key is present in config.
    XCG(p->xc, "DesignCapacity", &gm->design,   enum, bool_enum); // use design capacity?
    XCG(p->xc, "PollingTimeMs",  &gm->time,     int);             // polling interval
    XCG(p->xc, "TextSize",       &gm->textsize, str);             // Pango size string
    XCG(p->xc, "BatteryPath",    &gm->battery,  str);             // sysfs battery directory

    // Create the text label widget (initially empty; text_update() fills it).
    gm->main = gtk_label_new(NULL);

    // Perform an immediate update so the label is populated before the first timer tick.
    text_update(gm);

    // Add a 1-pixel border around the plugin container widget.
    gtk_container_set_border_width (GTK_CONTAINER (p->pwid), 1);

    // Pack the label into the plugin's container widget and show everything.
    gtk_container_add(GTK_CONTAINER(p->pwid), gm->main);
    gtk_widget_show_all(p->pwid);

    // Register the periodic polling timer.  The returned source ID must be
    // stored in gm->timer so it can be cancelled in batterytext_destructor().
    gm->timer = g_timeout_add((guint) gm->time,
        (GSourceFunc) text_update, (gpointer) gm);

    RET(1); // success
}


/*
 * class -- static plugin_class descriptor for the batterytext plugin.
 *
 * The framework uses this to find the constructor/destructor and to
 * determine how many bytes to allocate per instance (priv_size).
 */
static plugin_class class = {
    .count       = 0,                    // live instance counter (managed by framework)
    .type        = "batterytext",        // config file identifier
    .name        = "Generic Monitor",    // human-readable name
    .version     = "0.1",
    .description = "Display battery usage in text form",
    .priv_size   = sizeof(batterytext_priv), // framework allocates this per instance

    .constructor = batterytext_constructor,
    .destructor  = batterytext_destructor,
};

/* Exported symbol used by the fbpanel plugin loader to locate the class descriptor. */
static plugin_class *class_ptr = (plugin_class *) &class;
