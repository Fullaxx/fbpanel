/*
 * user.c -- fbpanel user plugin.
 *
 * Displays a user avatar image with a popup menu of user actions.
 * Delegates most UI work to the "menu" plugin (used as a helper via class_get).
 * Optionally fetches a Gravatar image for the user from gravatar.com.
 *
 * Functionality:
 *   1. Reads icon/image config to find the user avatar.
 *   2. Loads the "menu" plugin class and runs its constructor to build
 *      the icon+popup-menu widget in p->pwid.
 *   3. If "gravataremail" is configured, spawns wget to download the
 *      Gravatar avatar, then rebuilds the menu widget with the new image.
 *
 * Config keys:
 *   image         = /path/to/image.png   User avatar file path (optional).
 *   icon          = avatar-default       Icon theme name (optional; default if no image).
 *   gravataremail = user@example.com     Email for Gravatar fetch (optional).
 *   (all other menu plugin config keys are also supported, as menu is delegated to)
 *
 * Dependencies:
 *   - Requires the "menu" plugin to be available (class_get("menu")).
 *   - If "gravataremail" is set, requires "wget" to be on PATH.
 *
 * Security note (from original code): the Gravatar download path is hardcoded
 * to /tmp/gravatar — a shared, world-writable location.  This is unsafe on
 * multi-user systems (symlink attack, file replacement).
 */

#include "misc.h"
#include "run.h"
#include "../menu/menu.h"   /* menu_priv, menu_class — reuse menu as a helper */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>

//#define DEBUGPRN
#include "dbg.h"

/*
 * user_priv -- private data for one user plugin instance.
 *
 * Fields:
 *   chart - embedded menu_priv struct (MUST be first; used by "menu" constructor).
 *           NOTE: the field is named "chart" which is confusing — it should be "menu".
 *   dummy - unused alignment padding.
 *   sid   - GLib child-watch source ID for the wget process; 0 if not running.
 *           Removed in destructor via g_source_remove().
 *   pid   - GPid of the running wget process; 0 if not running.
 *           Killed with SIGKILL in destructor if still running.
 */
typedef struct {
    menu_priv chart;   /* embedded menu_priv (name is misleading; should be "menu") */
    gint dummy;        /* unused padding field */
    guint sid;         /* child-watch source ID; 0 = not active */
    GPid pid;          /* wget child PID; 0 = not running */
} user_priv;

/* Pointer to the "menu" plugin_class, acquired via class_get("menu") */
static menu_class *k;


/* Forward declaration needed because user_destructor references PLUGIN_CLASS(k) */
static void user_destructor(plugin_instance *p);

/* Maximum buffer length for the Gravatar URL (scheme + hash = well within 300 chars) */
#define GRAVATAR_LEN  300

/*
 * fetch_gravatar_done -- child-watch callback: wget has exited.
 *
 * Parameters:
 *   pid    - the PID that exited.
 *   status - exit status from waitpid (0 = success, non-zero = failure).
 *   data   - the plugin_instance* (also cast to user_priv*).
 *
 * If wget succeeded (status == 0), rebuilds the menu widget using the
 * downloaded Gravatar image at /tmp/gravatar.
 *
 * Memory: clears c->pid and c->sid to zero after reaping.
 *
 * BUG: The rebuild sequence is fragile:
 *   1. It reads image/icon from p->xc (non-owning pointers).
 *   2. Calls destructor then constructor on the "menu" helper.
 *   3. Then writes back icon/image with XCS.
 *   But the XCS after constructor may re-set fields that constructor already read,
 *   causing double-writes and potential leaks if the menu constructor strduped them.
 */
static void
fetch_gravatar_done(GPid pid, gint status, gpointer data)
{
    user_priv *c G_GNUC_UNUSED = data;   /* G_GNUC_UNUSED suppresses unused-variable warning */
    plugin_instance *p G_GNUC_UNUSED = data;
    gchar *image = NULL, *icon = NULL;

    ENTER;
    DBG("status %d\n", status);
    g_spawn_close_pid(c->pid);   /* release system resources for child PID */
    c->pid = 0;                  /* mark as no longer running */
    c->sid = 0;                  /* mark child-watch as removed */

    if (status)
        RET();                   /* wget failed; keep current avatar */

    DBG("rebuild menu\n");
    /* Read current icon/image from config (non-owning pointers from xconf) */
    XCG(p->xc, "icon", &icon, strdup);    /* get owned copy of icon name */
    XCG(p->xc, "image", &image, strdup);  /* get owned copy of image path */
    XCS(p->xc, "image", image, value);    /* write /tmp/gravatar path as new image */
    xconf_del(xconf_find(p->xc, "icon", 0), FALSE);  /* remove icon node (use image instead) */
    /* Rebuild the menu plugin widget in-place */
    PLUGIN_CLASS(k)->destructor(p);
    PLUGIN_CLASS(k)->constructor(p);
    /* Restore original icon/image values after rebuild */
    if (image) {
        XCS(p->xc, "image", image, value);
        g_free(image);
    }
    if (icon) {
        XCS(p->xc, "icon", icon, value);
        g_free(icon);
    }
    RET();
}


/*
 * fetch_gravatar -- GLib idle/timeout callback: start wget for Gravatar.
 *
 * Called once via g_timeout_add(300ms) from user_constructor when a
 * gravataremail is configured.  Computes the MD5 hash of the email address,
 * builds the Gravatar URL, and spawns wget to download it to /tmp/gravatar.
 *
 * Parameters:
 *   data - the plugin_instance* (also cast to user_priv*).
 *
 * Returns: FALSE (removes itself from the GLib main loop after one call).
 *
 * Security concern: the image is saved to /tmp/gravatar — a world-writable
 * path with a predictable name.  A local attacker could replace this file
 * with arbitrary content between the download and the read.
 * The original code comments: "FIXME: select more secure path".
 *
 * Memory: buf is stack-allocated; gravatar is a non-owning pointer from XCG(str).
 * The GChecksum is freed with g_checksum_free().
 */
static gboolean
fetch_gravatar(gpointer data)
{
    user_priv *c G_GNUC_UNUSED = data;
    plugin_instance *p G_GNUC_UNUSED = data;
    GChecksum *cs;
    gchar *gravatar = NULL;
    gchar buf[GRAVATAR_LEN];
    /* FIXME: /tmp/gravatar is an insecure shared path; use a per-user temp file */
    gchar *image = "/tmp/gravatar";
    gchar *argv[] = { "wget", "-q", "-O", image, buf, NULL };

    ENTER;
    cs = g_checksum_new(G_CHECKSUM_MD5);    /* MD5 hash for Gravatar URL */
    XCG(p->xc, "gravataremail", &gravatar, str);          /* non-owning pointer */
    g_checksum_update(cs, (guchar *) gravatar, -1);       /* hash the email string */
    snprintf(buf, sizeof(buf), "http://www.gravatar.com/avatar/%s",
        g_checksum_get_string(cs));
    g_checksum_free(cs);
    DBG("gravatar '%s'\n", buf);
    c->pid = run_app_argv(argv);                          /* spawn wget */
    c->sid = g_child_watch_add(c->pid, fetch_gravatar_done, data);  /* watch for completion */
    RET(FALSE);   /* one-shot; remove this timeout from the event loop */
}


/*
 * user_constructor -- initialise the user plugin.
 *
 * Loads the "menu" helper plugin class, sets a default avatar icon if no
 * image or icon is configured, runs the menu constructor to build the
 * pwid content, then schedules a Gravatar fetch if an email is configured.
 *
 * Parameters:
 *   p - the plugin_instance.
 *
 * Returns: 1 on success, 0 if the "menu" class is unavailable.
 *
 * Memory: k (menu_class*) is a static pointer shared across all user instances.
 *         This means only one user plugin per process can be active at a time
 *         reliably (k is overwritten on each constructor call).
 *         class_put("menu") is called in user_destructor.
 *
 * BUG: the g_timeout_add(300, fetch_gravatar, p) return value is not saved.
 *      If the plugin is destroyed before 300ms elapses, fetch_gravatar will
 *      fire with a dangling p pointer → use-after-free crash.
 */
static int
user_constructor(plugin_instance *p)
{
    user_priv *c G_GNUC_UNUSED = (user_priv *) p;
    gchar *image = NULL;
    gchar *icon = NULL;
    gchar *gravatar = NULL;

    ENTER;
    /* Load the "menu" plugin class (increments its reference count) */
    if (!(k = class_get("menu"))) {
        g_message("user: 'menu' plugin unavailable — plugin disabled");
        RET(0);
    }

    XCG(p->xc, "image", &image, str);   /* check for configured image path */
    XCG(p->xc, "icon", &icon, str);     /* check for configured icon name */
    if (!(image || icon))
        XCS(p->xc, "icon", "avatar-default", value);  /* default icon if nothing set */
    if (!PLUGIN_CLASS(k)->constructor(p)) {
        g_message("user: menu constructor failed — plugin disabled");
        RET(0);
    }

    XCG(p->xc, "gravataremail", &gravatar, str);
    DBG("gravatar email '%s'\n", gravatar);
    if (gravatar)
        /* BUG: timer ID not saved; can't cancel if plugin destroyed within 300ms */
        g_timeout_add(300, fetch_gravatar, p);

    gtk_widget_set_tooltip_markup(p->pwid, "<b>User</b>");
    RET(1);
}


/*
 * user_destructor -- clean up the user plugin.
 *
 * Order of operations:
 *   1. Call the "menu" helper's destructor to clean up the menu widget.
 *   2. Kill the wget child process if still running (SIGKILL).
 *   3. Remove the child-watch source if still active.
 *   4. Release the "menu" class reference.
 *
 * Parameters:
 *   p - the plugin_instance being destroyed.
 *
 * Note: After class_put("menu"), k (static pointer) may become invalid
 * if this was the last user plugin instance. This is safe here because
 * the destructor is the last thing that uses k.
 */
static void
user_destructor(plugin_instance *p)
{
    user_priv *c G_GNUC_UNUSED = (user_priv *) p;

    ENTER;
    PLUGIN_CLASS(k)->destructor(p);   /* clean up menu helper */
    if (c->pid)
        kill(c->pid, SIGKILL);        /* kill wget if still running */
    if (c->sid)
        g_source_remove(c->sid);      /* remove child-watch source */
    class_put("menu");                /* release "menu" class reference */
    RET();
}


/* plugin_class descriptor for the user plugin */
static plugin_class class = {
    .count       = 0,
    .type        = "user",
    .name        = "User menu",
    .version     = "1.0",
    .description = "User photo and menu of user actions",
    .priv_size   = sizeof(user_priv),

    .constructor = user_constructor,
    .destructor  = user_destructor,
};
/* Required for PLUGIN macro auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
