/*
 * os_linux.c.inc -- Linux-specific battery state reader for the battery plugin.
 *
 * This file is #included directly by battery.c when building on Linux.
 * It is NOT compiled as a standalone translation unit.
 *
 * Two backend strategies are attempted in sequence:
 *   1. Legacy /proc/acpi/battery/ interface (kernels before ~2.6.24).
 *   2. Modern /sys/class/power_supply/ interface (via power_supply.h API).
 * The first one to succeed wins; if both fail, c->exist is left FALSE.
 *
 * All functions in this file are static (translation-unit scope within
 * battery.c after inclusion).
 */

#include <string.h>
#include <ctype.h>

#include "power_supply.h"

/* Buffer size for string token comparisons (stack-allocated). */
#define LEN 100

/* Legacy /proc interface path; absent on modern kernels. */
#define PROC_ACPI "/proc/acpi/battery/"

/*
 * get_token_eq -- search a line buffer for a token and compare its value.
 *
 * Looks for the string `token` in `buf`, skips leading whitespace after
 * it, then compares the remaining text with `value`.
 *
 * Parameters:
 *   buf   -- null-terminated line buffer to search (not modified).
 *   token -- key string to locate within buf (e.g. "present:").
 *   value -- expected value string to compare against (e.g. "yes").
 *   ret   -- output: set to TRUE if the value matches, FALSE otherwise.
 *            Only written when the token is found.
 *
 * Returns: TRUE if the token was found (regardless of whether the value
 *          matched); FALSE if the token was not present in buf.
 *
 * Note: Uses strncmp limited to strlen(value), so partial prefix matches
 *   on longer values will return TRUE.
 */
static gboolean
get_token_eq(gchar *buf, gchar *token, gchar *value, gboolean *ret)
{
    int len;
    gchar *var; // pointer into buf, advanced past the token

    ENTER;
    len = strlen(token);

    // Find the token anywhere in the line.
    if (!(var = strstr(buf, token)))
        RET(FALSE); // token not present on this line

    // Skip past the token itself, then skip any whitespace (spaces, tabs).
    for (var = var + len; isspace(*var); var++) ;

    // Compare the next strlen(value) characters with the expected value.
    *ret = !strncmp(var, value, strlen(value));
    RET(TRUE);
}

/*
 * get_token_int -- search a line buffer for a token and parse its integer value.
 *
 * Looks for `token` in `buf`, skips whitespace, then parses one integer
 * with sscanf.
 *
 * Parameters:
 *   buf   -- null-terminated line buffer to search (not modified).
 *   token -- key string to locate within buf (e.g. "last full capacity:").
 *   value -- output: receives the parsed integer on success.
 *            Undefined if the function returns FALSE.
 *
 * Returns: TRUE if the token was found AND an integer was successfully
 *          parsed; FALSE otherwise.
 */
static gboolean
get_token_int(gchar *buf, gchar *token, gint *value)
{
    int len;
    gchar *var;

    ENTER;
    len = strlen(token);

    if (!(var = strstr(buf, token)))
        RET(FALSE); // token not present

    // Skip past token and any whitespace before the numeric value.
    for (var = var + len; isspace(*var); var++) ;

    // Parse exactly one decimal integer; sscanf returns count of items matched.
    if (sscanf(var, "%d", value) == 1)
        RET(TRUE);

    RET(FALSE); // no integer found after the token
}

/*
 * read_proc -- read battery info and state from the /proc/acpi/battery/<name>/ directory.
 *
 * Reads two files:
 *   <path>/info  -- contains "present:", "last full capacity:" lines.
 *   <path>/state -- contains "present:", "remaining capacity:", "charging state:" lines.
 *
 * Computes charge level as (remaining / last_full) * 100 and fills c->exist,
 * c->charging, and c->level.
 *
 * Parameters:
 *   c    -- battery_priv* for the plugin instance; c->exist, c->charging,
 *           and c->level are written on success.  Must not be NULL.
 *   path -- GString holding the base path up to and including the battery
 *           sub-directory name (e.g. "/proc/acpi/battery/BAT0").
 *           The function temporarily appends "/info" and "/state" and then
 *           restores the original length -- the GString's content is
 *           unchanged on return.
 *
 * Returns: TRUE if all reads succeeded and the battery is present and
 *          has valid capacity data; FALSE otherwise.
 *
 * Memory: g_file_get_contents() allocates `buf` on each call; freed with
 *   g_free() after use.  No persistent allocations are made.
 */
static gboolean
read_proc(battery_priv *c, GString *path)
{
    int len, lfcap, rcap;   // last full capacity, remaining capacity (integer mAh values)
    gchar *buf;             // heap-allocated file contents; freed after each file read
    gboolean ret, exist, charging;

    ENTER;
    len = path->len; // remember the base path length so we can restore it

    // --- Read the "info" file for battery presence and full capacity ---
    g_string_append(path, "/info");
    ret = g_file_get_contents(path->str, &buf, 0, NULL);
    DBG("reading %s %s\n", path->str, ret ? "ok" : "fail");
    g_string_truncate(path, len); // restore path to base directory

    if (!ret)
        RET(FALSE); // info file unreadable (battery absent or permission error)

    // Check "present: yes" and parse "last full capacity: <number>"
    ret = get_token_eq(buf, "present:", "yes", &exist)
        && exist && get_token_int(buf, "last full capacity:", &lfcap);

    g_free(buf); // release info file buffer
    if (!ret)
        RET(FALSE); // battery not present or capacity line missing

    // --- Read the "state" file for current charge and charging flag ---
    g_string_append(path, "/state");
    ret = g_file_get_contents(path->str, &buf, 0, NULL);
    DBG("reading %s %s\n", path->str, ret ? "ok" : "fail");
    g_string_truncate(path, len); // restore path again

    if (!ret)
        RET(FALSE);

    // Check presence again, get remaining capacity, and check if charging.
    ret = get_token_eq(buf, "present:", "yes", &exist)
        && exist
        && get_token_int(buf, "remaining capacity:", &rcap)
        && get_token_eq(buf, "charging state:", "charging", &charging);

    g_free(buf); // release state file buffer
    if (!ret)
        RET(FALSE);

    DBG("battery=%s\nlast full capacity=%d\nremaining capacity=%d\n"
        "charging=%d\n",
        path->str, lfcap, rcap, charging);

    // Sanity-check: remaining must not exceed full, full must be positive,
    // and remaining must be non-negative.
    if (!(lfcap >= rcap && lfcap > 0 && rcap >= 0))
        RET(FALSE);

    // Fill in the plugin's state fields.
    c->exist    = TRUE;
    c->charging = charging;
    // Integer division then cast: fine because lfcap > 0 is already guaranteed.
    c->level    = (int) ((gfloat) rcap * 100 / (gfloat) lfcap);

    RET(TRUE);
}

/*
 * battery_update_os_proc -- attempt to read battery state from /proc/acpi.
 *
 * Iterates the /proc/acpi/battery/ directory and tries read_proc() on
 * each sub-directory.  Stops on the first battery that reads successfully.
 * Only handles a single battery (the first successful one).
 *
 * Parameters:
 *   c -- battery_priv*; c->exist is cleared to FALSE at entry.
 *        c->exist, c->charging, and c->level are written if a battery is found.
 *
 * Returns: TRUE if a battery was successfully read; FALSE otherwise.
 *
 * Memory: GString `path` is heap-allocated and freed before returning.
 */
static gboolean
battery_update_os_proc(battery_priv *c)
{
    GString *path;     // reusable path builder
    int len;           // base path length (restored after each sub-directory attempt)
    GDir *dir;         // handle to the /proc/acpi/battery/ directory
    gboolean ret = FALSE;
    const gchar *file; // directory entry name (owned by GDir; do not free)

    ENTER;
    c->exist = FALSE; // assume no battery until proven otherwise

    path = g_string_sized_new(200); // pre-allocate for typical path lengths
    g_string_append(path, PROC_ACPI);
    len = path->len; // remember length of the base /proc/acpi/battery/ prefix

    // Open the legacy battery directory.
    if (!(dir = g_dir_open(path->str, 0, NULL))) {
        DBG("can't open dir %s\n", path->str);
        goto out; // directory absent (modern kernel) -- fall through to sysfs
    }

    // Iterate sub-directories (one per battery, e.g. "BAT0", "BAT1").
    while (!ret && (file = g_dir_read_name(dir))) {
        g_string_append(path, file);
        DBG("testing %s\n", path->str);
        // Only process actual directories (skip files).
        ret = g_file_test(path->str, G_FILE_TEST_IS_DIR);
        if (ret)
            ret = read_proc(c, path); // fills c->exist/charging/level on success
        g_string_truncate(path, len); // restore to PROC_ACPI base path
    }
    g_dir_close(dir);

 out:
    g_string_free(path, TRUE); // free GString and its character buffer
    RET(ret);
}

/*
 * battery_update_os_sys -- read battery state from /sys/class/power_supply/.
 *
 * Uses the power_supply API (power_supply.h) to enumerate all batteries
 * and AC adapters on the system.  Reports:
 *   c->exist    = TRUE if at least one battery is present.
 *   c->charging = TRUE if any AC adapter is online (proxy for "charging").
 *   c->level    = average capacity across all batteries (percent).
 *
 * Parameters:
 *   c -- battery_priv*; all three fields are written.
 *
 * Returns: c->exist (TRUE if a battery was found and read).
 *
 * Memory: power_supply_new() / power_supply_free() pair; no persistent
 *   allocations are made beyond the lifetime of this call.
 *
 * NOTE: c->charging is set to ac_online (AC adapter detected), which is
 *   only a proxy for "charging" -- a battery can be fully charged (Full)
 *   even while AC is connected.  The battery status field (e.g. "Full",
 *   "Charging", "Discharging") from the uevent file is not consulted here.
 *
 * NOTE: power_supply_get_bat_capacity() divides by bat_count, which is
 *   safe here because the check g_sequence_get_length(ps->bat_list) > 0
 *   ensures at least one battery exists before calling it.
 */
static gboolean
battery_update_os_sys(battery_priv *c)
{
    ENTER;
    c->exist = FALSE;

    // Allocate a fresh power_supply container (empty ac_list and bat_list).
    power_supply* ps = power_supply_new();

    // Scan /sys/class/power_supply/ and populate ac_list/bat_list.
    power_supply_parse(ps);

    if (g_sequence_get_length(ps->bat_list) > 0) {
        // At least one battery found: compute aggregate state.
        gboolean ac_online    = power_supply_is_ac_online(ps);    // TRUE if any adapter is online
        gdouble  bat_capacity = power_supply_get_bat_capacity(ps); // average % across all batteries

        c->exist    = TRUE;
        c->charging = ac_online;              // proxy: AC online => assume charging
        c->level    = (gfloat) bat_capacity;  // cast from double to float (minor precision loss)
    }

    // Release the power_supply container and all ac*/bat* elements.
    power_supply_free(ps);

    RET(c->exist);
}

/*
 * battery_update_os -- top-level Linux battery state reader.
 *
 * Tries the legacy /proc/acpi interface first; if that fails (e.g. the
 * directory does not exist on modern kernels), falls back to the sysfs
 * interface.  The first successful read populates c->exist/charging/level.
 *
 * Parameters:
 *   c -- battery_priv*; updated by whichever backend succeeds first.
 *
 * Returns: TRUE if any backend successfully read battery state; FALSE if
 *   neither backend found a usable battery (e.g. desktop system with no
 *   battery at all).
 *
 * This function is the sole entry point called from battery_update() in
 * battery.c.  Its name and signature are part of the platform abstraction
 * contract -- the non-Linux stub in battery.c has the same signature.
 */
static gboolean
battery_update_os(battery_priv *c)
{
    ENTER;
    // Short-circuit evaluation: if proc succeeds, sys is not called.
    RET(battery_update_os_proc(c) || battery_update_os_sys(c));
}
