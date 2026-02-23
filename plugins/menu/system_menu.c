/*
 * plugins/menu/system_menu.c -- XDG application-menu builder for fbpanel.
 *
 * PURPOSE
 * -------
 * Scans the XDG application directories (g_get_system_data_dirs() and
 * g_get_user_data_dir()) for *.desktop files and builds an xconf tree
 * that represents a categorised application menu.  The resulting tree is
 * consumed by menu_expand_xc() in menu.c whenever a <systemmenu> node
 * appears in the plugin configuration.
 *
 * OVERVIEW OF OPERATION
 * ---------------------
 * 1. xconf_new_from_systemmenu() creates one xconf "menu" node per category
 *    in main_cats[] and populates a GHashTable mapping category-name strings
 *    to those xconf nodes.
 * 2. do_app_dir() / do_app_dir_real() recursively walks the "applications"
 *    sub-directory of each XDG data directory.
 * 3. do_app_file() parses each *.desktop file and appends an <item> xconf
 *    node into the appropriate category menu.
 * 4. Empty categories are pruned and each category's items are sorted
 *    alphabetically by name.
 *
 * CHANGE DETECTION
 * ----------------
 * systemmenu_changed(btime) checks whether any *.desktop file or directory
 * has an mtime newer than @btime, indicating that a menu rebuild is needed.
 *
 * PUBLIC API
 * ----------
 *   xconf *xconf_new_from_systemmenu(void)
 *     Build and return a fresh xconf tree.  Caller owns the result and must
 *     free it with xconf_del(result, TRUE).
 *
 *   gboolean systemmenu_changed(time_t btime)
 *     Return TRUE if any XDG application directory or .desktop file has been
 *     modified after @btime.
 *
 * BUGS / ISSUES
 * -------------
 * See inline BUG/FIXME comments below.
 */


#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <time.h>

#include "panel.h"
#include "xconf.h"

//#define DEBUGPRN
#include "dbg.h"

/* GKeyFile group name for standard .desktop files. */
static const char desktop_ent[] = "Desktop Entry";

/* Name of the sub-directory within each XDG data dir that holds .desktop files. */
static const gchar app_dir_name[] = "applications";

/*
 * cat_info -- maps an XDG category name to a display icon and localised label.
 *
 * Fields:
 *   name       -- XDG category string (e.g. "AudioVideo"), used as hash key.
 *   icon       -- icon name in the GTK icon theme for the category submenu.
 *   local_name -- localised human-readable category name (via c_() macro).
 */
typedef struct {
    gchar *name;        /* XDG category identifier string; static lifetime */
    gchar *icon;        /* Theme icon name for this category; static lifetime */
    gchar *local_name;  /* Localised display name; static lifetime */
} cat_info;

/*
 * main_cats -- table of recognised XDG categories and their display metadata.
 *
 * Only applications whose "Categories" field matches at least one entry in
 * this table will appear in the generated menu.  Applications with categories
 * that match none of these entries are silently discarded.
 *
 * FIXME: The list is a hard-coded subset of the full XDG category spec.
 *        Many valid XDG categories (e.g. "Science", "Engineering") are
 *        silently dropped without feedback to the user.
 */
static cat_info main_cats[] = {
    { "AudioVideo", "applications-multimedia", c_("Audio & Video") },
    { "Education",  "applications-other",      c_("Education") },
    { "Game",       "applications-games",      c_("Game") },
    { "Graphics",   "applications-graphics",   c_("Graphics") },
    { "Network",    "applications-internet",   c_("Network") },
    { "Office",     "applications-office",     c_("Office") },
    { "Settings",   "preferences-system",      c_("Settings") },
    { "System",     "applications-system",     c_("System") },
    { "Utility",    "applications-utilities",  c_("Utilities") },
    { "Development","applications-development",c_("Development") },
};

/*
 * do_app_file -- parse a single .desktop file and add an item to the menu.
 *
 * Reads the [Desktop Entry] group from @file and extracts:
 *   Name       -- localised application name (key "Name").
 *   Exec       -- launch command (key "Exec"); field-code arguments (%f etc.)
 *                 are stripped by replacing "%" sequences with spaces.
 *   Icon       -- icon name or absolute path (key "Icon").
 *   Categories -- semicolon-separated list of XDG category strings.
 *
 * The file is SKIPPED (silently) when:
 *   - It cannot be parsed as a GKeyFile.
 *   - NoDisplay=true is set.
 *   - OnlyShowIn is present (regardless of its value).
 *   - "Exec" key is absent.
 *   - "Categories" key is absent or all categories are unrecognised.
 *   - "Name" key is absent.
 *
 * If a matching category is found the application is appended as an <item>
 * node inside that category's xconf <menu> node stored in @ht.
 *
 * Parameters:
 *   ht   -- GHashTable mapping category-name (const char *) -> (xconf *) for
 *           the category menu node.  Not modified (values are mutated in place).
 *   file -- path to the .desktop file to parse (relative or absolute).
 *
 * Memory notes:
 *   - name, icon, action, cats are all g_free/g_strfreev'd before return.
 *   - xconf nodes created here are owned by the tree rooted at the category
 *     mxc node in the hash table; no additional cleanup needed here.
 *
 * BUG: The Exec field-code stripping loop (while strchr) does not terminate
 *      if the last character of the string is '%' (i.e. a bare trailing
 *      percent with no following character).  dot[1] is checked for '\0' but
 *      after dot[0] = dot[1] = ' ' (both spaces) the loop restarts the search
 *      from the beginning of the string with strchr(action, '%'), not from
 *      dot+2, so it will re-find the same position, causing an infinite loop
 *      when "%%" appears in the Exec line.
 *
 * BUG: Icon extension stripping uses strcasecmp on dot+1; if the extension is
 *      "jpeg" or "xpm" it is not stripped, but those are also valid icon file
 *      types.  Only "png" and "svg" are handled.
 *
 * FIXME: OnlyShowIn is rejected unconditionally; it should check whether the
 *        current desktop environment is listed.
 */
static void
do_app_file(GHashTable *ht, const gchar *file)
{
    GKeyFile *f;
    gchar *name, *icon, *action, *dot;
    gchar **cats, **tmp;
    xconf *ixc, *vxc, *mxc;

    ENTER;
    DBG("desktop: %s\n", file);

    /* Initialise all pointers to NULL for safe goto-cleanup. */
    name = icon = action = dot = NULL;
    cats = tmp = NULL;

    f = g_key_file_new();

    /* Attempt to parse the .desktop file; skip on failure. */
    if (!g_key_file_load_from_file(f, file, 0, NULL))
        goto out;

    /* Skip entries that should not be displayed. */
    if (g_key_file_get_boolean(f, desktop_ent, "NoDisplay", NULL))
    {
        DBG("\tNoDisplay\n");
        goto out;
    }

    /* Skip entries restricted to specific desktop environments.
     * FIXME: should check whether the current DE is in the list. */
    if (g_key_file_has_key(f, desktop_ent, "OnlyShowIn", NULL))
    {
        DBG("\tOnlyShowIn\n");
        goto out;
    }

    /* Must have an Exec field to be useful. */
    if (!(action = g_key_file_get_string(f, desktop_ent, "Exec", NULL)))
    {
        DBG("\tNo Exec\n");
        goto out;
    }

    /* Must declare at least one category. */
    if (!(cats = g_key_file_get_string_list(f,
                desktop_ent, "Categories", NULL, NULL)))
    {
        DBG("\tNo Categories\n");
        goto out;
    }

    /* Must have a localised name. */
    if (!(name = g_key_file_get_locale_string(f,
                desktop_ent, "Name", NULL, NULL)))
    {
        DBG("\tNo Name\n");
        goto out;
    }

    /* Icon is optional; absence is logged but not fatal. */
    icon = g_key_file_get_string(f, desktop_ent, "Icon", NULL);
    if (!icon)
        DBG("\tNo Icon\n");

    /* Strip Exec field-code arguments (%f, %u, %F, %U, etc.) by replacing
     * the "%" and the following character with spaces.
     *
     * BUG: if action contains "%%" this loop is infinite because after
     *      replacing "%%"->"  " strchr will find the first space again from
     *      the start of the string.  The search should resume from dot+2. */
    while ((dot = strchr(action, '%'))) {
        if (dot[1] != '\0')
            dot[0] = dot[1] = ' ';
    }
    DBG("action: %s\n", action);

    /* If icon is a filename with a recognised extension but not an absolute
     * path, strip the extension so that the theme lookup uses only the stem. */
    if (icon && icon[0] != '/' && (dot = strrchr(icon, '.')) &&
        !(strcasecmp(dot + 1, "png") && strcasecmp(dot + 1, "svg")))
    {
        *dot = '\0'; /* truncate at the dot */
    }
    DBG("icon: %s\n", icon);

    /* Find the first recognised category from the application's category list. */
    for (mxc = NULL, tmp = cats; *tmp; tmp++)
        if ((mxc = g_hash_table_lookup(ht, *tmp)))
            break;
    if (!mxc)
    {
        /* All categories are unrecognised; drop the application. */
        DBG("\tUnknown categories\n");
        goto out;
    }

    /* Build the <item> xconf node and append to the matched category menu. */
    ixc = xconf_new("item", NULL);
    xconf_append(mxc, ixc);

    if (icon)
    {
        /* Use "image" key for absolute paths, "icon" key for theme names. */
        vxc = xconf_new((icon[0] == '/') ? "image" : "icon", icon);
        xconf_append(ixc, vxc);
    }
    vxc = xconf_new("name", name);
    xconf_append(ixc, vxc);
    vxc = xconf_new("action", action);
    xconf_append(ixc, vxc);

out:
    g_free(icon);
    g_free(name);
    g_free(action);
    g_strfreev(cats);    /* frees the NULL-terminated string array */
    g_key_file_free(f);
}

/*
 * do_app_dir_real -- recursively scan a directory for .desktop files.
 *
 * Changes the process working directory to @dir, iterates its entries, and
 * for each:
 *   - Sub-directories: recurses with do_app_dir_real().
 *   - Files ending in ".desktop": calls do_app_file().
 *   - All other files: ignored.
 *
 * The working directory is restored to its value before the call returns.
 *
 * Parameters:
 *   ht  -- GHashTable of category-name -> xconf category node.
 *   dir -- directory path to scan (relative to CWD at call time is fine
 *          because the function chdirs into it).
 *
 * Memory notes:
 *   cwd is freed before return; @ht and the xconf nodes it points to are
 *   not freed here (owned by the caller of xconf_new_from_systemmenu).
 *
 * BUG: The function uses g_chdir() to traverse directories, which changes the
 *      process-wide working directory.  This is not thread-safe.  Any other
 *      thread that relies on the CWD during this scan will see an incorrect
 *      directory.  The code tries to restore CWD on exit, but if do_app_file()
 *      or a recursive call leaves the CWD in an unexpected state (e.g. the
 *      dir was deleted during traversal), the restore may silently fail.
 */
static void
do_app_dir_real(GHashTable *ht, const gchar *dir)
{
    GDir *d = NULL;
    gchar *cwd;
    const gchar *name;

    ENTER;
    DBG("%s\n", dir);

    /* Save CWD so we can restore it before returning. */
    cwd = g_get_current_dir();

    if (g_chdir(dir))
    {
        DBG("can't chdir to %s\n", dir);
        goto out;
    }
    if (!(d = g_dir_open(".", 0, NULL)))
    {
        ERR("can't open dir %s\n", dir);
        goto out;
    }

    while ((name = g_dir_read_name(d)))
    {
        if (g_file_test(name, G_FILE_TEST_IS_DIR))
        {
            do_app_dir_real(ht, name); /* recurse into sub-directory */
            continue;
        }
        if (!g_str_has_suffix(name, ".desktop"))
            continue; /* skip non-.desktop files */
        do_app_file(ht, name);
    }

out:
    if (d)
        g_dir_close(d);
    /* Restore CWD regardless of how we got here. */
    g_chdir(cwd);
    g_free(cwd);
    RET();
}

/*
 * do_app_dir -- process one XDG data directory for application entries.
 *
 * Guard function around do_app_dir_real().  Maintains a visited-directory
 * set inside @ht to avoid processing the same directory twice (e.g. when the
 * same path appears in both system and user data dirs).
 *
 * NOTE: The visited-directory set reuses the same @ht hash table that maps
 * category names to xconf nodes.  The directory path is used as the key and
 * @ht itself as the value.  This is a hash-table abuse; it works only because
 * the key space (absolute directory paths) does not overlap with the category
 * name keys (short strings like "AudioVideo").  If a directory name ever
 * matched a category name, the lookup would return the wrong type.
 *
 * Parameters:
 *   ht  -- GHashTable used both as the category map and the visited-dir set.
 *   dir -- absolute path to the XDG data directory to process.
 *
 * Memory notes:
 *   @dir is used as a hash key but NOT copied; the caller must ensure @dir
 *   remains valid while @ht is alive.  This is fine because the XDG data
 *   directory strings come from g_get_system_data_dirs() / g_get_user_data_dir()
 *   which have process lifetime.
 *
 * BUG: Same thread-safety issue as do_app_dir_real() -- uses g_chdir().
 *
 * FIXME: Mixing category-name -> xconf* and dir-path -> ht* entries in the
 *        same hash table is fragile.  A separate GHashTable for visited dirs
 *        would be cleaner.
 */
static void
do_app_dir(GHashTable *ht, const gchar *dir)
{
    gchar *cwd;

    ENTER;
    cwd = g_get_current_dir();
    DBG("%s\n", dir);

    /* Check whether we have already visited this directory. */
    if (g_hash_table_lookup(ht, dir))
    {
        DBG("already visited\n");
        goto out;
    }

    /* Mark directory as visited by inserting a sentinel value (ht itself). */
    g_hash_table_insert(ht, (gpointer) dir, ht);

    if (g_chdir(dir))
    {
        ERR("can't chdir to %s\n", dir);
        goto out;
    }

    /* Scan the "applications" subdirectory within this data dir. */
    do_app_dir_real(ht, app_dir_name);

out:
    g_chdir(cwd);
    g_free(cwd);
    RET();
}

/*
 * xconf_cmp_names -- GCompareFunc: compare two xconf nodes by their "name" child.
 *
 * Used with g_slist_sort() to sort category or item lists alphabetically.
 *
 * Parameters:
 *   a, b -- pointers to (xconf *) nodes to compare.
 *
 * Returns:
 *   Negative, zero, or positive as strcmp would.
 *   Returns 0 if either node has no "name" child (g_strcmp0 handles NULL).
 */
static int
xconf_cmp_names(gpointer a, gpointer b)
{
    xconf *aa = a, *bb = b;
    gchar *s1 = NULL, *s2 = NULL;
    int ret;

    ENTER;
    XCG(aa, "name", &s1, str);
    XCG(bb, "name", &s2, str);
    ret = g_strcmp0(s1, s2);
    DBG("cmp %s %s - %d\n", s1, s2, ret);
    RET(ret);
}

/*
 * dir_changed -- check whether a directory (or any .desktop file in it) has
 *               been modified after @btime.
 *
 * Recursively descends into sub-directories and stats each .desktop file.
 * Returns TRUE as soon as a modification is detected (short-circuit).
 *
 * Parameters:
 *   dir   -- directory path to check (chdir is used internally).
 *   btime -- baseline timestamp (seconds since epoch).
 *
 * Returns:
 *   TRUE  -- at least one directory or .desktop file has mtime > btime.
 *   FALSE -- nothing newer found, or the directory could not be opened.
 *
 * BUG: Same g_chdir() thread-safety issue as do_app_dir_real().
 *
 * BUG: ctime (st_ctime) is printed in debug output but only st_mtime is
 *      compared.  ctime changes on permission/ownership updates, which might
 *      also warrant a rebuild.  The inconsistency is misleading.
 *
 * FIXME: The early-return path ("if (ret = buf.st_mtime > btime) return TRUE")
 *        returns before restoring the CWD.  This is safe because the CWD has
 *        NOT yet been changed at that point, but it is confusing style.
 */
static gboolean
dir_changed(const gchar *dir, time_t btime)
{
    GDir *d = NULL;
    gchar *cwd;
    const gchar *name;
    gboolean ret = FALSE;
    struct stat buf;

    ENTER;
    DBG("%s\n", dir);

    /* Stat the directory itself first; skip if it does not exist. */
    if (g_stat(dir, &buf))
        return FALSE; /* directory not accessible; assume unchanged */

    DBG("dir=%s ct=%lu mt=%lu\n", dir, buf.st_ctime, buf.st_mtime);

    /* If the directory mtime itself is newer, we are done immediately.
     * Note: CWD has not been changed yet at this early-return point. */
    if ((ret = buf.st_mtime > btime))
        return TRUE;

    /* Save CWD before changing into the target directory. */
    cwd = g_get_current_dir();
    if (g_chdir(dir))
    {
        DBG("can't chdir to %s\n", dir);
        goto out;
    }
    if (!(d = g_dir_open(".", 0, NULL)))
    {
        ERR("can't open dir %s\n", dir);
        goto out;
    }

    while (!ret && (name = g_dir_read_name(d)))
    {
        if (g_file_test(name, G_FILE_TEST_IS_DIR))
        {
            /* Recurse; ret becomes TRUE if any nested file is newer. */
            ret = dir_changed(name, btime);
        }
        else if (!g_str_has_suffix(name, ".desktop"))
            continue; /* ignore non-.desktop files */
        else if (g_stat(name, &buf))
            continue; /* skip unreadable files */

        DBG("name=%s ct=%lu mt=%lu\n", name, buf.st_ctime, buf.st_mtime);
        ret = buf.st_mtime > btime;
    }

out:
    if (d)
        g_dir_close(d);
    g_chdir(cwd);
    g_free(cwd);
    RET(ret);
}

/*
 * systemmenu_changed -- check whether any XDG application directory has changed.
 *
 * Iterates over all system XDG data directories and the user data directory,
 * checking their "applications" sub-directory for modifications newer than
 * @btime.
 *
 * Parameters:
 *   btime -- the menu-build timestamp to compare against.
 *
 * Returns:
 *   TRUE  -- at least one .desktop file or directory is newer than @btime.
 *   FALSE -- nothing has changed.
 *
 * Side effects:
 *   Temporarily modifies the process CWD; restores it before returning.
 *
 * BUG: g_chdir() return values in this function are not checked; if a chdir
 *      fails, the subsequent dir_changed() call runs against the wrong directory.
 */
gboolean
systemmenu_changed(time_t btime)
{
    const gchar * const * dirs;
    gboolean ret = FALSE;
    gchar *cwd = g_get_current_dir(); /* save CWD for restoration */

    /* Check all system data directories. */
    for (dirs = g_get_system_data_dirs(); *dirs && !ret; dirs++)
    {
        /* BUG: g_chdir() return value not checked. */
        g_chdir(*dirs);
        ret = dir_changed(app_dir_name, btime);
    }

    DBG("btime=%lu\n", btime);

    /* Check the user data directory if no change found in system dirs. */
    if (!ret)
    {
        /* BUG: g_chdir() return value not checked. */
        g_chdir(g_get_user_data_dir());
        ret = dir_changed(app_dir_name, btime);
    }

    /* Restore CWD. */
    g_chdir(cwd);
    g_free(cwd);
    return ret;
}

/*
 * xconf_new_from_systemmenu -- build a complete system application menu xconf tree.
 *
 * Creates one <menu> xconf node for each entry in main_cats[], scans all XDG
 * application directories for .desktop files, assigns applications to their
 * categories, removes empty categories, and sorts the result.
 *
 * Returns:
 *   A newly allocated xconf tree rooted at a "systemmenu" node.
 *   Caller owns the result and must eventually free it with xconf_del(result, TRUE).
 *
 * Memory notes:
 *   - All xconf nodes (mxc, tmp, ixc, vxc) become children of the returned
 *     tree and are freed by xconf_del(result, TRUE).
 *   - @ht maps category-name strings (static lifetime) to xconf* pointers
 *     that are part of the returned tree; @ht is destroyed before return,
 *     which does NOT free the keys or values (the GHashTable does not own them
 *     because no destroy functions were given to g_hash_table_new()).
 *   - Categories deleted in the "retry" loop via xconf_del(tmp, FALSE) --
 *     FALSE means children are freed recursively but the parent link is not
 *     updated by xconf_del; the outer loop relies on restarting from the head
 *     each time to avoid a dangling-pointer walk.
 *
 * BUG: The "retry" loop pattern (goto retry after xconf_del) is correct but
 *      O(n^2) in the number of categories.  For the current small main_cats[]
 *      table this is acceptable, but it would not scale.
 *
 * FIXME: The GHashTable is created without value/key destroy functions.  If
 *        do_app_dir() inserts a visited-directory sentinel entry (dir -> ht),
 *        those entries leak when g_hash_table_destroy() is called (the keys are
 *        stack/static strings so they do not actually leak memory, but the
 *        conceptual design is fragile).
 */
xconf *
xconf_new_from_systemmenu()
{
    xconf *xc, *mxc, *tmp;
    GSList *w;
    GHashTable *ht;
    int i;
    const gchar * const * dirs;

    /* ---- Phase 1: Create empty category menu nodes ---- */
    ht = g_hash_table_new(g_str_hash, g_str_equal);
    xc = xconf_new("systemmenu", NULL);

    for (i = 0; i < G_N_ELEMENTS(main_cats); i++)
    {
        mxc = xconf_new("menu", NULL);
        xconf_append(xc, mxc);

        /* Add localised name node as child of the category menu. */
        tmp = xconf_new("name", _(main_cats[i].local_name));
        xconf_append(mxc, tmp);

        /* Add icon node for the category. */
        tmp = xconf_new("icon", main_cats[i].icon);
        xconf_append(mxc, tmp);

        /* Register this category xconf node in the lookup table. */
        g_hash_table_insert(ht, main_cats[i].name, mxc);
    }

    /* ---- Phase 2: Populate categories from .desktop files ---- */
    for (dirs = g_get_system_data_dirs(); *dirs; dirs++)
        do_app_dir(ht, *dirs);
    do_app_dir(ht, g_get_user_data_dir());

    /* ---- Phase 3: Remove empty categories ---- */
    /* Restart iteration from the head after each deletion (avoid dangling ptr).
     * O(n^2) but acceptable for the small number of categories. */
retry:
    for (w = xc->sons; w; w = g_slist_next(w))
    {
        tmp = w->data;
        if (!xconf_find(tmp, "item", 0)) /* no <item> children -> empty category */
        {
            xconf_del(tmp, FALSE); /* remove this category node from the tree */
            goto retry;
        }
    }

    /* ---- Phase 4: Sort categories and items alphabetically ---- */
    xc->sons = g_slist_sort(xc->sons, (GCompareFunc) xconf_cmp_names);
    for (w = xc->sons; w; w = g_slist_next(w))
    {
        tmp = w->data;
        /* Sort items within each category. */
        tmp->sons = g_slist_sort(tmp->sons, (GCompareFunc) xconf_cmp_names);
    }

    /* Destroy the hash table; keys/values are owned externally so no-op here. */
    g_hash_table_destroy(ht);

    return xc;
}
