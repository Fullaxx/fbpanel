/*
 * battery.c -- fbpanel plugin: battery charge level indicator using icons.
 *
 * Displays the current battery charge level using a set of themed icons
 * via the "meter" plugin class.  The charge level and charging state are
 * read from sysfs (/sys/class/power_supply/) or the legacy /proc/acpi/
 * interface (via os_linux.c.inc on Linux).
 *
 * Plugin lifecycle:
 *   battery_constructor() -- called once; sets up the timer.
 *   battery_update()      -- called every 2000 ms by the GLib main loop.
 *   battery_destructor()  -- called once; removes the timer, tears down meter.
 *
 * Timer management:
 *   c->timer holds the GSource ID returned by g_timeout_add().
 *   It is removed in battery_destructor() with g_source_remove().
 *   The timer ID is checked for non-zero before removal as a safety guard.
 */

#include "misc.h"
#include "../meter/meter.h"
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

//#define DEBUGPRN
#include "dbg.h"

/* Pointer to the "meter" plugin class vtable.  Set once in
 * battery_constructor() via class_get("meter").  Held for the lifetime
 * of the plugin instance; released with class_put("meter") in the
 * destructor. */
static meter_class *k;

/*
 * battery_priv -- private per-instance data for the battery plugin.
 *
 * Layout note: meter_priv must be the FIRST field so that a battery_priv*
 * can be safely cast to both plugin_instance* (via meter_priv->plugin)
 * and meter_priv*.  The fbpanel plugin system relies on this struct layout
 * guarantee.
 *
 * Timer: c->timer stores the g_timeout_add() source ID.
 *   Must be removed in battery_destructor() via g_source_remove(c->timer).
 */
typedef struct {
    meter_priv meter;   // MUST be first: superclass data (plugin_instance embedded inside)
    int timer;          // GLib timer source ID from g_timeout_add(); 0 if not started
    gfloat level;       // current battery level in percent (0.0 .. 100.0)
    gboolean charging;  // TRUE if battery is currently charging (AC online)
    gboolean exist;     // TRUE if at least one battery was detected
} battery_priv;

/* Forward declaration: platform-specific battery state reader.
 * Implemented in os_linux.c.inc (included below) on Linux, or as a
 * stub on other platforms. */
static gboolean battery_update_os(battery_priv *c);

/*
 * batt_working -- NULL-terminated array of icon names for discharging state.
 * Icons are indexed 0 (empty) through 8 (full) based on the charge level.
 * Icon names must exist in the current GTK icon theme.
 */
static gchar *batt_working[] = {
    "battery_0",
    "battery_1",
    "battery_2",
    "battery_3",
    "battery_4",
    "battery_5",
    "battery_6",
    "battery_7",
    "battery_8",
    NULL  // sentinel: terminates the array
};

/*
 * batt_charging -- NULL-terminated array of icon names for charging state.
 * Same index scheme as batt_working but uses charging-specific icons.
 */
static gchar *batt_charging[] = {
    "battery_charging_0",
    "battery_charging_1",
    "battery_charging_2",
    "battery_charging_3",
    "battery_charging_4",
    "battery_charging_5",
    "battery_charging_6",
    "battery_charging_7",
    "battery_charging_8",
    NULL  // sentinel
};

/*
 * batt_na -- NULL-terminated array containing a single icon for the
 * "no battery / running on AC only" state.
 */
static gchar *batt_na[] = {
    "battery_na",
    NULL  // sentinel
};

/* Include the Linux-specific battery reading implementation.
 * On Linux this defines battery_update_os() which reads from sysfs or /proc/acpi.
 * On other platforms the else-branch below provides a stub that sets c->exist = FALSE. */
#if defined __linux__
#include "os_linux.c.inc"
#else

/* Stub for non-Linux platforms: mark battery as absent. */
static void
battery_update_os(battery_priv *c)
{
    c->exist = FALSE;
}

#endif

/*
 * battery_update -- periodic callback: refresh battery state and update the display.
 *
 * Called every 2000 ms by the GLib timer registered in battery_constructor().
 * Reads the current battery state from OS interfaces, selects the appropriate
 * icon set, updates the tooltip markup, and tells the meter class to refresh
 * the displayed icon.
 *
 * Parameters:
 *   c -- battery_priv* cast to void* by GLib; the actual instance data.
 *        Must not be NULL.
 *
 * Returns: TRUE to keep the timer firing (GSourceFunc contract).
 *          If FALSE were returned, the timer would be automatically removed
 *          and c->timer would become a dangling source ID.
 */
static gboolean
battery_update(battery_priv *c)
{
    gchar buf[50]; // tooltip markup buffer (stack-allocated, 50 bytes is sufficient)
    gchar **i;     // pointer into one of the icon name arrays

    ENTER;
    battery_update_os(c); // platform-specific: fills c->exist, c->charging, c->level

    if (c->exist) {
        // Battery found: choose icon set based on whether we are charging.
        i = c->charging ? batt_charging : batt_working;

        // Build tooltip like "<b>Battery:</b> 72%\nCharging" (no suffix when discharging).
        g_snprintf(buf, sizeof(buf), "<b>Battery:</b> %d%%%s",
            (int) c->level, c->charging ? "\nCharging" : "");
        // Set tooltip on the plugin's top-level widget (cast through plugin_instance*).
        gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    } else {
        // No battery detected (desktop/AC-only system).
        i = batt_na;
        gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid,
            "Runing on AC\nNo battery found"); // NOTE: "Runing" is a pre-existing typo
    }

    // Delegate icon selection and level display to the meter class.
    k->set_icons(&c->meter, i);         // switch the icon set (charging/discharging/na)
    k->set_level(&c->meter, c->level);  // select the specific icon for the current level

    RET(TRUE); // keep timer alive
}


/*
 * battery_constructor -- fbpanel plugin constructor for the battery plugin.
 *
 * Initialises the plugin by:
 *   1. Obtaining the "meter" plugin class via class_get().
 *   2. Calling the meter constructor to create the GtkImage widget.
 *   3. Registering a 2-second periodic timer for battery_update().
 *   4. Running an immediate update so the icon is visible before the first tick.
 *
 * Parameters:
 *   p -- plugin_instance* allocated by the fbpanel framework (size = priv_size).
 *        Must not be NULL.
 *
 * Returns: 1 on success, 0 on failure (class_get or meter constructor failed).
 *
 * Timer: c->timer receives the GSource ID from g_timeout_add().
 *   This ID MUST be passed to g_source_remove() in battery_destructor().
 *   Failure to do so leaks the timer and causes use-after-free callbacks
 *   after the plugin is destroyed.
 *
 * Memory: Does not allocate anything beyond what the framework and meter
 *   class manage.  class_put("meter") must balance class_get("meter").
 */
static int
battery_constructor(plugin_instance *p)
{
    battery_priv *c;

    ENTER;
    // Obtain the meter plugin class vtable; increments the class reference count.
    // Must be balanced by class_put("meter") in battery_destructor().
    if (!(k = class_get("meter")))
        RET(0); // "meter" plugin not loaded or unavailable

    // Initialise the embedded meter (creates GtkImage and sets up icon infrastructure).
    if (!PLUGIN_CLASS(k)->constructor(p))
        RET(0); // meter constructor failed (e.g. widget creation failed)

    // Cast p to our private type; safe because battery_priv begins with meter_priv
    // which begins with plugin_instance.
    c = (battery_priv *) p;

    // Register a 2-second (2000 ms) repeating timer.
    // The returned source ID is stored so the destructor can cancel it.
    c->timer = g_timeout_add(2000, (GSourceFunc) battery_update, c);

    // Perform an immediate update so the display is populated before the first tick.
    battery_update(c);

    RET(1); // success
}

/*
 * battery_destructor -- fbpanel plugin destructor for the battery plugin.
 *
 * Tears down the plugin in reverse construction order:
 *   1. Cancels the periodic timer (prevents callbacks after destruction).
 *   2. Destroys the meter widget via the meter class destructor.
 *   3. Releases the meter class reference (balances class_get in constructor).
 *
 * Parameters:
 *   p -- plugin_instance* (same pointer passed to battery_constructor).
 *        Must not be NULL.
 *
 * Timer cleanup: g_source_remove(c->timer) cancels the GLib timer.
 *   The guard (c->timer != 0) is a safety check; g_source_remove(0) would
 *   attempt to remove a nonexistent source and print a GLib warning.
 *
 * Signal cleanup: No g_signal_connect() calls are made in this plugin
 *   (signals are managed by the meter class; its destructor handles them).
 */
static void
battery_destructor(plugin_instance *p)
{
    battery_priv *c = (battery_priv *) p;

    ENTER;
    // Cancel the periodic timer before any other teardown to prevent
    // battery_update() from being called after the plugin data is freed.
    if (c->timer)
        g_source_remove(c->timer); // remove GLib timer; c->timer is now a dangling ID

    // Destroy the meter widget and release its resources.
    PLUGIN_CLASS(k)->destructor(p);

    // Decrement the "meter" class reference count (balances class_get in constructor).
    class_put("meter");

    RET();
}

/*
 * class -- static plugin_class descriptor for the battery plugin.
 *
 * Registered as a plugin via class_ptr below.  The framework uses this
 * table to find the constructor, destructor, and private-data size.
 *
 * priv_size: The framework allocates priv_size bytes and passes a pointer
 *   to that allocation as plugin_instance* to the constructor.  Because
 *   battery_priv begins with meter_priv (which begins with plugin_instance),
 *   the cast chain battery_priv* -> meter_priv* -> plugin_instance* is safe.
 */
static plugin_class class = {
    .count       = 0,              // number of live instances (managed by framework)
    .type        = "battery",      // unique plugin identifier used in config files
    .name        = "battery usage",
    .version     = "1.1",
    .description = "Display battery usage",
    .priv_size   = sizeof(battery_priv), // framework allocates this many bytes per instance
    .constructor = battery_constructor,
    .destructor  = battery_destructor,
};

/* Exported symbol used by the fbpanel plugin loader to find the class descriptor. */
static plugin_class *class_ptr = (plugin_class *) &class;
