/**
 * @file
 * @brief fbpanel plugin: battery charge level indicator using themed icons.
 *
 * Displays the current battery charge level and charging state using a set
 * of themed icons rendered through the "meter" plugin class. Battery state
 * is read via the legacy /proc/acpi/battery/ interface first, falling back
 * to the modern /sys/class/power_supply/ interface (via os_linux.c.inc on
 * Linux), polled every 2000 ms.
 *
 * @par Plugin lifecycle
 *   - battery_constructor() -- called once; sets up the timer.
 *   - battery_update()      -- called every 2000 ms by the GLib main loop.
 *   - battery_destructor()  -- called once; removes the timer, tears down
 *     the meter.
 *
 * @note Timer management: c->timer holds the GSource ID returned by
 *       g_timeout_add(). It is removed in battery_destructor() with
 *       g_source_remove(), guarded by a non-zero check before removal.
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


/**
 * @brief Constructor for the battery plugin.
 *
 * Initialises the plugin by:
 *   -# Obtaining the "meter" plugin class via class_get().
 *   -# Calling the meter constructor to create the GtkImage widget.
 *   -# Registering a 2-second periodic timer for battery_update().
 *   -# Running an immediate update so the icon is visible before the first
 *      tick.
 *
 * @param p plugin_instance* allocated by the fbpanel framework (size =
 *          priv_size). Must not be NULL.
 * @return 1 on success, 0 on failure (class_get() or the meter constructor
 *         failed).
 * @note c->timer receives the GSource ID from g_timeout_add(). This ID
 *       MUST be passed to g_source_remove() in battery_destructor();
 *       failure to do so leaks the timer and causes use-after-free
 *       callbacks after the plugin is destroyed.
 * @note Does not allocate anything beyond what the framework and meter
 *       class manage. class_put("meter") must balance class_get("meter").
 */
static int
battery_constructor(plugin_instance *p)
{
    battery_priv *c;

    ENTER;
    // Obtain the meter plugin class vtable; increments the class reference count.
    // Must be balanced by class_put("meter") in battery_destructor().
    if (!(k = class_get("meter"))) {
        g_message("battery: 'meter' plugin unavailable — plugin disabled");
        RET(0);
    }
    // Initialise the embedded meter (creates GtkImage and sets up icon infrastructure).
    if (!PLUGIN_CLASS(k)->constructor(p)) {
        g_message("battery: meter constructor failed — plugin disabled");
        RET(0);
    }

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

/**
 * @brief Destructor for the battery plugin.
 *
 * Tears down the plugin in reverse construction order:
 *   -# Cancels the periodic timer (prevents callbacks after destruction).
 *   -# Destroys the meter widget via the meter class destructor.
 *   -# Releases the meter class reference (balances class_get() in the
 *      constructor).
 *
 * @param p plugin_instance* (same pointer passed to battery_constructor()).
 *          Must not be NULL.
 * @note g_source_remove(c->timer) cancels the GLib timer; the guard
 *       (c->timer != 0) avoids passing 0 to g_source_remove(), which would
 *       attempt to remove a nonexistent source and print a GLib warning.
 * @note No g_signal_connect() calls are made in this plugin -- signals are
 *       managed by the meter class, whose destructor handles them.
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
