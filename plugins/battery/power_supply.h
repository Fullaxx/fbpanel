// Time-stamp: < power_supply.h (2015-12-04 19:01) >

#ifndef POWER_SUPPLY_H
#define POWER_SUPPLY_H

#include <glib-2.0/glib.h>

/*
 * Struct representing a single AC (mains) power supply adapter.
 *
 * Memory ownership: All pointer fields (path, name) are heap-allocated
 * strings owned by this struct. They must be freed with g_free() before
 * or during ac_free(). The struct itself is freed by ac_free().
 *
 * Lifecycle: Created by ac_new(), destroyed by ac_free() which is
 * registered as the GSequence element destructor in power_supply_new().
 * Callers must NOT free ac structs individually after handing them to a
 * GSequence -- g_sequence_free() will call ac_free() automatically.
 */
typedef struct {
    /* Full filesystem path to the uevent file for this adapter,
     * e.g. "/sys/class/power_supply/AC/uevent".
     * Heap-allocated; freed by ac_free(). */
    gchar* path;

    /* Human-readable name from POWER_SUPPLY_NAME key in uevent,
     * e.g. "AC". May be NULL if the key was absent.
     * Heap-allocated; freed by ac_free(). */
    gchar* name;

    /* TRUE if this adapter is currently supplying power
     * (POWER_SUPPLY_ONLINE == "1" in uevent). */
    gboolean online;
} ac;

/*
 * Struct representing a single battery power supply.
 *
 * Memory ownership: All pointer fields (path, name, status) are
 * heap-allocated strings owned by this struct. They must be freed
 * with g_free() before or during bat_free(). The struct itself is
 * freed by bat_free().
 *
 * Lifecycle: Created by bat_new(), destroyed by bat_free() which is
 * registered as the GSequence element destructor in power_supply_new().
 * Callers must NOT free bat structs individually after handing them to a
 * GSequence -- g_sequence_free() will call bat_free() automatically.
 */
typedef struct {
    /* Full filesystem path to the uevent file for this battery,
     * e.g. "/sys/class/power_supply/BAT0/uevent".
     * Heap-allocated; freed by bat_free(). */
    gchar* path;

    /* Human-readable name from POWER_SUPPLY_NAME, e.g. "BAT0".
     * May be NULL if the key was absent.
     * Heap-allocated; freed by bat_free(). */
    gchar* name;

    /* Charging status string from POWER_SUPPLY_STATUS,
     * e.g. "Charging", "Discharging", "Full".
     * May be NULL if the key was absent.
     * Heap-allocated; freed by bat_free(). */
    gchar* status;

    /* Remaining charge expressed as a percentage in the range 0.0..100.0.
     * Initialised to -1.0 to signal "unknown/not yet read".
     * On modern kernels this comes directly from POWER_SUPPLY_CAPACITY;
     * on older kernels it is computed as
     *   (ENERGY_NOW / ENERGY_FULL) * 100  or
     *   (CHARGE_NOW / CHARGE_FULL) * 100. */
    gdouble capacity;
} bat;

/*
 * Aggregate container for all power supplies found on the system.
 *
 * Memory ownership: This struct owns both GSequence objects.
 * Each GSequence owns its elements (ac* / bat* structs) and will
 * invoke the registered destructor (ac_free / bat_free) on each
 * element when g_sequence_free() is called.
 *
 * Lifecycle: Allocated by power_supply_new(), freed by power_supply_free().
 * Callers are responsible for calling power_supply_free() exactly once.
 */
typedef struct {
    /* Ordered list of ac* elements, one per AC adapter found under
     * /sys/class/power_supply/.  May be empty but never NULL after
     * power_supply_new(). */
    GSequence* ac_list;

    /* Ordered list of bat* elements, one per battery found under
     * /sys/class/power_supply/.  May be empty but never NULL after
     * power_supply_new(). */
    GSequence* bat_list;
} power_supply;

/*
 * power_supply_new -- allocate and initialise a power_supply container.
 *
 * Returns: A newly heap-allocated power_supply with empty ac_list and
 *          bat_list GSequences.  Never returns NULL (g_new aborts on OOM).
 *
 * Memory: Caller owns the returned pointer and MUST eventually pass it to
 *         power_supply_free() to release all resources.
 */
power_supply* power_supply_new();

/*
 * power_supply_free -- release all memory owned by a power_supply.
 *
 * Parameters:
 *   p -- gpointer (actually power_supply*) previously returned by
 *        power_supply_new().  Passing NULL is undefined behaviour.
 *
 * Memory: Frees the power_supply struct, both GSequences, and every
 *         ac*/bat* element they contain (via ac_free/bat_free callbacks
 *         registered at sequence creation time).
 *
 * Note: Signature uses gpointer so this function can be used directly
 *       as a GDestroyNotify callback (e.g. in a GSequence or GHashTable).
 */
void power_supply_free(gpointer p);

/*
 * power_supply_parse -- populate a power_supply by scanning the sysfs tree.
 *
 * Walks /sys/class/power_supply/, reads the "type" file for each entry,
 * then reads the "uevent" file to fill in an ac or bat struct which is
 * appended to ps->ac_list or ps->bat_list respectively.
 *
 * Parameters:
 *   ps -- previously allocated power_supply (from power_supply_new()).
 *         Must not be NULL.  Any existing list contents are preserved
 *         (entries are only appended, never cleared -- callers that want
 *         a fresh parse should use a fresh power_supply_new()).
 *
 * Returns: The same ps pointer that was passed in (for chaining).
 *
 * NOTE: The public header declares this as power_supply_parse() with no
 *       parameters, but the implementation takes a power_supply* argument.
 *       This is a header/implementation mismatch -- see BUGS section.
 */
power_supply* power_supply_parse();

/*
 * power_supply_is_ac_online -- test whether any AC adapter is supplying power.
 *
 * Iterates ps->ac_list and returns TRUE as soon as one adapter whose
 * online field is TRUE is found.
 *
 * Parameters:
 *   ps -- populated power_supply; must not be NULL.
 *
 * Returns: TRUE if at least one AC adapter is online, FALSE otherwise
 *          (including when ac_list is empty).
 */
gboolean power_supply_is_ac_online(power_supply* ps);

/*
 * power_supply_get_bat_capacity -- compute average battery capacity.
 *
 * Sums the capacity field of every battery in ps->bat_list whose
 * capacity > 0, then divides by the total number of batteries
 * (including those with capacity <= 0).
 *
 * Parameters:
 *   ps -- populated power_supply; must not be NULL.
 *
 * Returns: Average capacity as a percentage (0.0..100.0).
 *
 * WARNING: Division by zero if bat_list is empty (bat_count == 0).
 *          See BUGS section.
 */
gdouble power_supply_get_bat_capacity(power_supply* ps);

#endif /* POWER_SUPPLY_H */
