// Time-stamp: < power_supply.c (2015-12-04 19:01) >

#include <string.h>
#include <glib-2.0/glib.h>
#include <glib-2.0/glib/gprintf.h>

#include "power_supply.h"

/* Set to non-zero to enable verbose debug output to stderr. */
#define DEBUG 0

/* Maximum expected length for uevent keys and values.
 * Used as the initial GString allocation hint; GStrings grow dynamically
 * so this is an optimisation hint, not a hard limit. */
#define STRING_LEN 100

/* Base sysfs directory containing one sub-directory per power supply. */
#define SYS_ACPI_PATH "/sys/class/power_supply/"

/* File inside each supply sub-directory that identifies the supply type. */
#define SYS_ACPI_TYPE_FILE "type"

/* Contents of the "type" file that identifies an AC adapter.
 * Note the trailing newline -- g_file_get_contents() preserves it. */
#define SYS_ACPI_TYPE_AC "Mains\n"

/* Contents of the "type" file that identifies a battery. */
#define SYS_ACPI_TYPE_BAT "Battery\n"

/* File inside each supply sub-directory holding key=value pairs. */
#define SYS_ACPI_UEVENT_FILE "uevent"

/* uevent key for the human-readable supply name (e.g. "AC", "BAT0"). */
#define SYS_ACPI_UEVENT_NAME_KEY "POWER_SUPPLY_NAME"

/* uevent key for AC online status; value is "0" or "1". */
#define SYS_ACPI_UEVENT_AC_ONLINE_KEY "POWER_SUPPLY_ONLINE"

/* The value indicating that AC is currently supplying power. */
#define SYS_ACPI_UEVENT_AC_ONLINE_VALUE "1"

/* uevent key for battery charging state (e.g. "Charging", "Discharging"). */
#define SYS_ACPI_UEVENT_BAT_STATUS_KEY "POWER_SUPPLY_STATUS"

/* uevent key present on recent kernels: direct capacity percentage. */
#define SYS_ACPI_UEVENT_BAT_CAPACITY_KEY "POWER_SUPPLY_CAPACITY"

/*
 * Older kernel fallback keys -- capacity must be computed as:
 *   POWER_SUPPLY_ENERGY_NOW / POWER_SUPPLY_ENERGY_FULL  (AC off), or
 *   POWER_SUPPLY_CHARGE_NOW / POWER_SUPPLY_CHARGE_FULL  (AC on).
 * Both pairs are in micro-Wh or micro-Ah respectively; units cancel out
 * in the division so the ratio is unitless.
 */
#define SYS_ACPI_UEVENT_BAT_ENERGY_FULL_KEY "POWER_SUPPLY_ENERGY_FULL"
#define SYS_ACPI_UEVENT_BAT_ENERGY_NOW_KEY  "POWER_SUPPLY_ENERGY_NOW"
#define SYS_ACPI_UEVENT_BAT_CHARGE_FULL_KEY "POWER_SUPPLY_CHARGE_FULL"
#define SYS_ACPI_UEVENT_BAT_CHARGE_NOW_KEY  "POWER_SUPPLY_CHARGE_NOW"

/*
 * uevent_ghfunc -- GHFunc callback for g_hash_table_foreach().
 *
 * Prints one key => value pair from the uevent hash table to stderr.
 * Only invoked when DEBUG is non-zero.
 *
 * Parameters:
 *   key       -- gpointer to a gchar* hash key (const, owned by hash table)
 *   value     -- gpointer to a gchar* hash value (const, owned by hash table)
 *   user_data -- unused (pass NULL)
 */
static void
uevent_ghfunc(gpointer key, gpointer value, gpointer user_data)
{
    gchar* k = (gchar*) key;
    gchar* v = (gchar*) value;
    g_fprintf(stderr, "'%s' => '%s'\n", k, v);
}

/*
 * uevent_parse -- read a sysfs uevent file and return its contents as a hash.
 *
 * The uevent file has the format:
 *   KEY=VALUE\n
 *   KEY=VALUE\n
 *   ...
 * Each line is split at the first '=' character; the key and value are
 * stored as separate heap-allocated strings inside the returned GHashTable.
 *
 * Parameters:
 *   filename -- absolute path to the uevent file (e.g.
 *               "/sys/class/power_supply/BAT0/uevent").
 *               Must not be NULL.
 *
 * Returns: A newly allocated GHashTable mapping gchar* -> gchar* on success,
 *          or NULL if the file does not exist or cannot be read.
 *
 * Memory ownership: The caller owns the returned GHashTable and is
 *   responsible for destroying it with g_hash_table_destroy().
 *   Both keys and values inside the table are heap-allocated duplicates
 *   and are freed automatically by the GHashTable destructor
 *   (g_free is registered for both key and value at creation time).
 */
static GHashTable*
uevent_parse(gchar* filename)
{
    GHashTable* hash = NULL;                        // result, NULL = not yet allocated
    GString* key   = g_string_sized_new(STRING_LEN); // accumulator for the current key token
    GString* value = g_string_sized_new(STRING_LEN); // accumulator for the current value token
    gchar* buf = NULL;                              // raw file contents (heap-allocated by glib)
    guint buf_len = 0;                              // total byte count of buf
    gchar c;                                        // current character being processed
    guint i;                                        // loop index over buf
    gboolean equals_sign_found = FALSE;             // TRUE after '=' seen in current line

    // Only attempt to open the file if it is a regular file (not a dir/symlink/etc.)
    if (g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
        // Read the entire file into buf; glib allocates buf and null-terminates it.
        // The second size_t* argument (0) means "don't return length", not "read 0 bytes".
        if (g_file_get_contents(filename, &buf, 0, NULL)) {
            // Create the hash table; both keys and values are owned by the table
            // and freed with g_free() when the table is destroyed.
            hash = g_hash_table_new_full(&g_str_hash, &g_str_equal, &g_free, &g_free);
            buf_len = strlen(buf);

            // Character-by-character scan of the file contents.
            for (i = 0; i < buf_len; ++i) {
                c = buf[i];
                if (c == '=' && !equals_sign_found) {
                    // First '=' on this line marks the boundary between key and value.
                    equals_sign_found = TRUE;
                } else if (c == '\n' && equals_sign_found) {
                    // End of a complete KEY=VALUE line: store it and reset accumulators.
                    equals_sign_found = FALSE;
                    // g_strdup() copies key->str and value->str; the table owns those copies.
                    g_hash_table_insert(hash, g_strdup(key->str), g_strdup(value->str));
                    g_string_truncate(key, 0);   // reset key accumulator for next line
                    g_string_truncate(value, 0); // reset value accumulator for next line
                } else {
                    // Append current character to whichever token we are currently building.
                    if (equals_sign_found) {
                        g_string_append_c(value, c);
                    } else {
                        g_string_append_c(key, c);
                    }
                }
            }
            // Debug: dump all parsed key-value pairs to stderr.
            if (DEBUG) {
                g_hash_table_foreach(hash, &uevent_ghfunc, NULL);
            }
        }
    }

    // Release raw file buffer regardless of parse outcome.
    g_free(buf);
    // Free GString objects and their internal character buffers (TRUE = free the char data).
    g_string_free(key, TRUE);
    g_string_free(value, TRUE);

    return hash; // NULL if the file could not be read
}

/*
 * ac_new -- allocate and zero-initialise an ac struct.
 *
 * Parameters:
 *   path -- heap-allocated string with the full path to this adapter's
 *           uevent file.  Ownership is TRANSFERRED to the returned struct;
 *           the caller must NOT free path afterwards.
 *
 * Returns: Newly allocated ac*. Never returns NULL (g_new aborts on OOM).
 *
 * Memory: Caller takes ownership of the returned pointer. It should be
 *   handed to a GSequence (via g_sequence_append) so that ac_free() is
 *   called automatically; or freed directly via ac_free().
 */
static ac*
ac_new(gchar* path)
{
    ac* tmp = g_new(ac, 1);
    tmp->path   = path;   // takes ownership of caller's heap string
    tmp->name   = NULL;   // populated later by ac_parse()
    tmp->online = FALSE;  // populated later by ac_parse()
    return tmp;
}

/*
 * ac_free -- release all resources owned by an ac struct.
 *
 * Parameters:
 *   p -- gpointer (actually ac*) to the struct to free.
 *        Must not be NULL.
 *
 * Memory: Frees tmp->path, tmp->name (both may be NULL, which g_free handles),
 *   and finally the struct itself.
 *
 * Note: Signature uses gpointer so this can serve as a GDestroyNotify
 *   callback registered with g_sequence_new().
 */
static void
ac_free(gpointer p)
{
    ac* tmp = (ac*) p;
    g_free(tmp->path);  // free the uevent path string
    g_free(tmp->name);  // free the name string (g_free(NULL) is safe)
    g_free(tmp);        // free the struct itself
    if (DEBUG) {
        g_fprintf(stderr, "ac_free %p\n", p);
    }
}

/*
 * ac_print -- GFunc callback for g_sequence_foreach(); prints an ac to stderr.
 *
 * Parameters:
 *   p         -- gpointer (actually ac*) to the element to print.
 *   user_data -- unused (pass NULL).
 *
 * Note: Only called when DEBUG is non-zero.
 */
static void
ac_print(gpointer p, gpointer user_data)
{
    ac* tmp = (ac*) p;
    g_fprintf(stderr, "AC\n  path: %s\n  name: %s\n online: %d\n",
        tmp->path, tmp->name, tmp->online);
}

/*
 * ac_parse -- fill in the name and online fields of an ac struct.
 *
 * Reads ac->path (the uevent file), parses it with uevent_parse(), and
 * extracts the POWER_SUPPLY_NAME and POWER_SUPPLY_ONLINE keys.
 *
 * Parameters:
 *   ac -- pointer to an ac struct whose ->path field has been set.
 *         Must not be NULL.  ->name and ->online are written on success.
 *
 * Returns: The same ac pointer that was passed in (for chaining).
 *
 * Memory: Allocates ac->name via g_strdup(); freed by ac_free().
 *   The temporary GHashTable is destroyed before returning.
 */
static ac*
ac_parse(ac* ac)
{
    GHashTable* hash;
    gchar* tmp_value;

    if (ac->path != NULL) {
        hash = uevent_parse(ac->path); // parse uevent file into a key-value map
        if (hash != NULL) {
            // Extract the supply name (POWER_SUPPLY_NAME=<name>)
            tmp_value = (gchar*) g_hash_table_lookup(hash, SYS_ACPI_UEVENT_NAME_KEY);
            if (tmp_value != NULL) {
                ac->name = g_strdup(tmp_value); // duplicate: hash table retains original
            }

            // Extract online flag (POWER_SUPPLY_ONLINE=0 or 1)
            tmp_value = (gchar*) g_hash_table_lookup(hash, SYS_ACPI_UEVENT_AC_ONLINE_KEY);
            if (tmp_value != NULL) {
                // strcmp returns 0 (falsy) on match, so negate to get a boolean TRUE for "1".
                ac->online = strcmp(SYS_ACPI_UEVENT_AC_ONLINE_VALUE, tmp_value) == 0;
            }

            // Destroy the hash table and its contents (g_free applied to every key/value).
            g_hash_table_destroy(hash);
        }
    }

    return ac;
}

/*
 * bat_new -- allocate and zero-initialise a bat struct.
 *
 * Parameters:
 *   path -- heap-allocated string with the full path to this battery's
 *           uevent file.  Ownership is TRANSFERRED to the returned struct.
 *
 * Returns: Newly allocated bat*. Never returns NULL (g_new aborts on OOM).
 *
 * Memory: See ac_new(); same ownership contract.
 */
static bat*
bat_new(gchar* path)
{
    bat* tmp = g_new(bat, 1);
    tmp->path     = path;  // takes ownership of caller's heap string
    tmp->name     = NULL;  // populated later by bat_parse()
    tmp->status   = NULL;  // populated later by bat_parse()
    tmp->capacity = -1;    // -1 signals "not yet determined"
    return tmp;
}

/*
 * bat_free -- release all resources owned by a bat struct.
 *
 * Parameters:
 *   p -- gpointer (actually bat*) to the struct to free.  Must not be NULL.
 *
 * Memory: Frees tmp->path, tmp->name, tmp->status (all may be NULL, which
 *   g_free handles safely), and finally the struct itself.
 *
 * Note: Registered as the GDestroyNotify for ps->bat_list in
 *   power_supply_new() so it is called automatically by g_sequence_free().
 */
static void
bat_free(gpointer p)
{
    bat* tmp = (bat*) p;
    g_free(tmp->path);   // free the uevent path string
    g_free(tmp->name);   // free the name string (g_free(NULL) is safe)
    g_free(tmp->status); // free the status string
    g_free(tmp);         // free the struct itself
    if (DEBUG) {
        g_fprintf(stderr, "bat_free %p\n", p);
    }
}

/*
 * bat_print -- GFunc callback for g_sequence_foreach(); prints a bat to stderr.
 *
 * Parameters:
 *   p         -- gpointer (actually bat*) to the element to print.
 *   user_data -- unused (pass NULL).
 *
 * Note: Only called when DEBUG is non-zero.
 */
static void
bat_print(gpointer p, gpointer user_data)
{
    bat* tmp = (bat*) p;
    g_fprintf(stderr,
        "BATTERY\n  path: %s\n  name: %s\n  status: %s\n  capacity: %f\n",
        tmp->path, tmp->name, tmp->status, tmp->capacity);
}

/*
 * bat_parse -- fill in the name, status, and capacity fields of a bat struct.
 *
 * Reads bat->path (the uevent file), parses it with uevent_parse(), and
 * extracts relevant keys in this priority order for capacity:
 *   1. POWER_SUPPLY_CAPACITY  (modern kernels, direct percentage)
 *   2. POWER_SUPPLY_ENERGY_NOW / POWER_SUPPLY_ENERGY_FULL  (older kernels)
 *   3. POWER_SUPPLY_CHARGE_NOW / POWER_SUPPLY_CHARGE_FULL  (older kernels)
 *
 * Parameters:
 *   bat -- pointer to a bat struct whose ->path has been set.  Must not be NULL.
 *
 * Returns: The same bat pointer (for chaining).
 *
 * Memory: Allocates bat->name and bat->status via g_strdup(); freed by bat_free().
 *
 * WARNING: Division by zero is possible if ENERGY_FULL or CHARGE_FULL is zero
 *   (see BUGS section).  The guard "tmp > 0" only protects against a zero
 *   numerator, not a zero denominator.
 */
static bat*
bat_parse(bat* bat)
{
    GHashTable* hash;
    gchar* tmp_value;

    if (bat->path != NULL) {
        hash = uevent_parse(bat->path); // parse uevent file into a key-value map
        if (hash != NULL) {
            // Extract the supply name (POWER_SUPPLY_NAME=<name>)
            tmp_value = (gchar*) g_hash_table_lookup(hash, SYS_ACPI_UEVENT_NAME_KEY);
            if (tmp_value != NULL) {
                bat->name = g_strdup(tmp_value);
            }

            // Extract the human-readable charging state string
            tmp_value = (gchar*) g_hash_table_lookup(hash, SYS_ACPI_UEVENT_BAT_STATUS_KEY);
            if (tmp_value != NULL) {
                bat->status = g_strdup(tmp_value);
            }

            // --- Capacity determination (three-tier fallback) ---

            // Tier 1: Modern kernels expose a direct percentage value.
            tmp_value = (gchar*) g_hash_table_lookup(hash, SYS_ACPI_UEVENT_BAT_CAPACITY_KEY);
            if (tmp_value != NULL) {
                bat->capacity = g_ascii_strtod(tmp_value, NULL);
            } else {
                // Tier 2: Older kernels -- try energy-based ratio.
                tmp_value = (gchar*) g_hash_table_lookup(hash, SYS_ACPI_UEVENT_BAT_ENERGY_NOW_KEY);
                gdouble tmp = -1;
                if (tmp_value != NULL) {
                    // AC adapter is off (kernel provides ENERGY_NOW when discharging)
                    tmp = g_ascii_strtod(tmp_value, NULL);
                    tmp_value = (gchar*) g_hash_table_lookup(hash, SYS_ACPI_UEVENT_BAT_ENERGY_FULL_KEY);
                    // BUG: guard checks tmp > 0 (numerator), but does not check
                    // that ENERGY_FULL > 0 (denominator). Division by zero possible.
                    if (tmp_value != NULL && tmp > 0) {
                        tmp = tmp / g_ascii_strtod(tmp_value, NULL) * 100;
                        bat->capacity = tmp;
                    }
                } else {
                    // Tier 3: Older kernels -- try charge-based ratio.
                    tmp_value = (gchar*) g_hash_table_lookup(hash, SYS_ACPI_UEVENT_BAT_CHARGE_NOW_KEY);
                    if (tmp_value != NULL) {
                        // AC adapter is on (kernel provides CHARGE_NOW when charging)
                        tmp = g_ascii_strtod(tmp_value, NULL);
                        tmp_value = (gchar*) g_hash_table_lookup(hash, SYS_ACPI_UEVENT_BAT_CHARGE_FULL_KEY);
                        // BUG: same denominator-zero issue as the ENERGY path above.
                        if (tmp_value != NULL && tmp > 0) {
                            tmp = tmp / g_ascii_strtod(tmp_value, NULL) * 100;
                            bat->capacity = tmp;
                        }
                    }
                }
            }

            // Release the uevent hash table and all its key/value strings.
            g_hash_table_destroy(hash);
        }
    }

    return bat;
}

/*
 * power_supply_new -- allocate and initialise a power_supply container.
 *
 * Creates an empty power_supply with two GSequences configured to call
 * ac_free() / bat_free() automatically on each element when the sequence
 * is destroyed.
 *
 * Returns: Newly heap-allocated power_supply*. Never NULL (g_new aborts on OOM).
 *
 * Memory: Caller owns the returned pointer; must call power_supply_free() to
 *   release it and all contained ac* and bat* elements.
 */
extern power_supply*
power_supply_new() {
    power_supply* tmp = g_new(power_supply, 1);
    // Register ac_free/bat_free as the element destructor for each sequence.
    // These will be invoked automatically by g_sequence_free().
    tmp->ac_list  = g_sequence_new(&ac_free);
    tmp->bat_list = g_sequence_new(&bat_free);
    return tmp;
}

/*
 * power_supply_free -- release all resources owned by a power_supply.
 *
 * Calls g_sequence_free() on both sequences, which in turn calls ac_free()
 * and bat_free() on every element (freeing their path/name/status strings
 * and the element structs themselves), then frees the power_supply struct.
 *
 * Parameters:
 *   p -- gpointer (actually power_supply*) returned by power_supply_new().
 *        Must not be NULL.
 *
 * Note: Signature is gpointer so this can be used as a GDestroyNotify.
 */
extern void
power_supply_free(gpointer p) {
    power_supply* tmp = (power_supply*) p;
    g_sequence_free(tmp->ac_list);  // frees all ac* elements via ac_free()
    g_sequence_free(tmp->bat_list); // frees all bat* elements via bat_free()
    g_free(tmp);                    // free the container struct itself
    if (DEBUG) {
        g_fprintf(stderr, "power_supply_free %p\n", p);
    }
}

/*
 * power_supply_parse -- populate a power_supply by scanning sysfs.
 *
 * Iterates over every entry in /sys/class/power_supply/, reads the "type"
 * file to determine whether it is an AC adapter or a battery, then reads
 * the corresponding "uevent" file and appends an ac or bat struct to the
 * appropriate list in ps.
 *
 * Parameters:
 *   ps -- pointer to a previously created power_supply.  Must not be NULL.
 *         Note: entries are APPENDED; existing list contents are not cleared.
 *
 * Returns: ps (for convenience chaining).
 *
 * Memory:
 *   - Temporary GString for path construction is freed before returning.
 *   - Each ac* and bat* entry is heap-allocated and transferred to the GSequence.
 *   - The "contents" buffer from g_file_get_contents() is freed after each use.
 *
 * NOTE: The public header declares power_supply_parse() with no parameters,
 *   but this implementation requires a power_supply* argument.  This is a
 *   header/implementation mismatch (see BUGS section).
 */
extern power_supply*
power_supply_parse(power_supply* ps) {
    GDir* dir = NULL;
    const gchar* tmp;                               // directory entry name (owned by GDir)
    GString* filename = g_string_sized_new(STRING_LEN); // reusable path builder
    guint len = 0;                                  // length of path up to (but not including) filename
    gchar* contents;                                // file contents buffer (heap, from g_file_get_contents)

    // Only proceed if the sysfs power supply directory exists.
    if (g_file_test(SYS_ACPI_PATH, G_FILE_TEST_IS_DIR)) {
        dir = g_dir_open(SYS_ACPI_PATH, 0, NULL);
        if (dir != NULL) {
            // Iterate over each sub-directory (e.g. "AC", "BAT0", "BAT1", ...).
            while ((tmp = g_dir_read_name(dir)) != NULL) {
                // Build the path to the "type" file: /sys/class/power_supply/<name>/type
                g_string_append(filename, SYS_ACPI_PATH);
                g_string_append(filename, tmp);
                g_string_append_c(filename, G_DIR_SEPARATOR);
                len = filename->len; // remember length up to the trailing slash
                g_string_append(filename, SYS_ACPI_TYPE_FILE);

                if (g_file_test(filename->str, G_FILE_TEST_IS_REGULAR)) {
                    // Read the "type" file to decide which struct to create.
                    g_file_get_contents(filename->str, &contents, 0, NULL);

                    // Reuse the filename buffer: replace "type" with "uevent".
                    g_string_truncate(filename, len);
                    g_string_append(filename, SYS_ACPI_UEVENT_FILE);

                    if (strcmp(SYS_ACPI_TYPE_AC, contents) == 0) {
                        // AC adapter found: create an ac struct, parse it, append to ac_list.
                        // g_strdup transfers a new heap copy of the uevent path to ac_new().
                        ac* tmp = ac_new(g_strdup(filename->str));
                        ac_parse(tmp);
                        g_sequence_append(ps->ac_list, tmp); // sequence takes ownership
                    } else if (strcmp(SYS_ACPI_TYPE_BAT, contents) == 0) {
                        // Battery found: create a bat struct, parse it, append to bat_list.
                        bat* tmp = bat_new(g_strdup(filename->str));
                        bat_parse(tmp);
                        g_sequence_append(ps->bat_list, tmp); // sequence takes ownership
                    } else {
                        // Unknown type: log a warning (no newline -- minor formatting issue).
                        g_fprintf(stderr, "unsupported power supply type %s", contents);
                    }
                    g_free(contents); // free the "type" file buffer
                }
                // Reset the path builder for the next directory entry.
                g_string_truncate(filename, 0);
            }
            g_dir_close(dir);
        }
    }

    // Free the reusable path-building GString and its underlying character buffer.
    g_string_free(filename, TRUE);

    // Debug: dump all discovered supplies to stderr.
    if (DEBUG) {
        g_sequence_foreach(ps->ac_list,  &ac_print,  NULL);
        g_sequence_foreach(ps->bat_list, &bat_print, NULL);
    }

    return ps;
}

/*
 * power_supply_is_ac_online -- test whether any AC adapter is supplying power.
 *
 * Short-circuits on the first online adapter found.
 *
 * Parameters:
 *   ps -- populated power_supply; must not be NULL.
 *
 * Returns: TRUE if at least one ac struct has online == TRUE.
 */
extern gboolean
power_supply_is_ac_online(power_supply* ps)
{
    gboolean ac_online = FALSE;  // pessimistic default: assume not on AC
    GSequenceIter* it;
    ac* ac_power;

    if (ps->ac_list != NULL) {
        it = g_sequence_get_begin_iter(ps->ac_list); // first element (or end if empty)
        while (!g_sequence_iter_is_end(it)) {
            ac_power = (ac*) g_sequence_get(it);
            if (ac_power->online) {
                ac_online = TRUE; // found at least one online adapter
                break;
            }
            it = g_sequence_iter_next(it);
        }
    }
    return ac_online;
}

/*
 * power_supply_get_bat_capacity -- compute the average battery capacity.
 *
 * Iterates all bat entries, sums capacities where capacity > 0, then
 * divides by the total number of batteries.
 *
 * Parameters:
 *   ps -- populated power_supply; must not be NULL.
 *
 * Returns: Average capacity as a percentage (0.0..100.0), or 0.0 / 0 (NaN)
 *   if there are no batteries.
 *
 * WARNING (high severity): If bat_list is empty, bat_count remains 0 and
 *   the final return statement performs a floating-point division by zero,
 *   yielding +Inf or NaN.  The calling code in battery_update_os_sys()
 *   partially guards against this by checking
 *   g_sequence_get_length(ps->bat_list) > 0 before calling this function,
 *   but the function itself provides no such guard.  See BUGS section.
 *
 * NOTE: Batteries with capacity == 0 are excluded from the sum but still
 *   counted in the denominator (bat_count).  This means a dead battery
 *   (0%) artificially lowers the reported average.
 */
extern gdouble
power_supply_get_bat_capacity(power_supply* ps)
{
    gdouble total_bat_capacity = 0; // running sum of individual capacities
    guint bat_count = 0;            // total number of bat entries seen (denominator)
    GSequenceIter* it;
    bat* battery;

    if (ps->bat_list != NULL) {
        it = g_sequence_get_begin_iter(ps->bat_list);
        while (!g_sequence_iter_is_end(it)) {
            battery = (bat*) g_sequence_get(it);
            // Only add capacity to the sum if it is positive (excludes
            // the -1 sentinel and 0%, but 0% batteries still increment bat_count).
            if (battery->capacity > 0) {
                total_bat_capacity = total_bat_capacity + battery->capacity;
            }
            bat_count++; // count every battery regardless of its capacity value
            it = g_sequence_iter_next(it);
        }
    }
    // BUG: bat_count may be 0 here, causing division by zero.
    return total_bat_capacity / bat_count;
}
