// Time-stamp: < main.c (2015-12-04 19:02) >
// run with: make -k -f Makefile-test valgrind
//
// main.c -- standalone test driver for the power_supply parsing library.
//
// This file is NOT part of the fbpanel plugin itself.  It is a standalone
// executable used for unit-testing the power_supply_* API outside of the
// panel context (see Makefile-test).
//
// Build and run with Valgrind to check for memory leaks:
//   make -k -f Makefile-test valgrind
//
// Expected output on a laptop with one battery at 80% on AC power:
//   ac_online: 1
//   bat_capacity: 80.000000

#include <glib-2.0/glib.h>
#include <glib-2.0/glib/gprintf.h>

#include "power_supply.h"

/*
 * main -- entry point for the power_supply test driver.
 *
 * Allocates a power_supply, parses the current system's power supplies,
 * queries AC online status and average battery capacity, then frees
 * all allocated memory before printing the results.
 *
 * Parameters:
 *   argc -- argument count (unused)
 *   args -- argument vector (unused)
 *
 * Returns: 0 always (no error reporting on failure to find supplies).
 *
 * Memory:
 *   ps is allocated by power_supply_new() and freed by power_supply_free().
 *   ac_online and bat_capacity are value types (gboolean, gdouble) and
 *   require no cleanup.
 *
 * NOTE: power_supply_free(ps) is called BEFORE the g_fprintf() output.
 *   This is safe because ac_online and bat_capacity are value types copied
 *   off the heap before the free.  No dangling pointer access occurs.
 *
 * WARNING: If no batteries are present, power_supply_get_bat_capacity()
 *   will divide by zero (bat_count == 0) returning NaN/+Inf.  The
 *   g_fprintf call will then print an implementation-defined float string.
 *   See BUGS section in power_supply.c.
 */
int main(int argc, char** args)
{
    // Allocate an empty power_supply container (ac_list and bat_list are empty).
    power_supply* ps = power_supply_new();

    // Walk /sys/class/power_supply/ and populate ps->ac_list and ps->bat_list.
    power_supply_parse(ps);

    // Query results before freeing (results are value types, not pointers into ps).
    gboolean ac_online    = power_supply_is_ac_online(ps);       // TRUE if any AC adapter is on
    gdouble  bat_capacity = power_supply_get_bat_capacity(ps);   // average battery % (or NaN if no batteries)

    // Release all heap memory: ps, both GSequences, and all ac*/bat* elements.
    power_supply_free(ps); // ps is now invalid; do not dereference after this line

    // Print results to stdout (ac_online as integer 0/1, bat_capacity as float).
    g_fprintf(stdout, "ac_online: %d\nbat_capacity: %f\n", ac_online, bat_capacity);

    return 0;
}
