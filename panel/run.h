/*
 * run.h -- Asynchronous process execution API for fbpanel.
 *
 * Provides simple wrappers around GLib's g_spawn_* family for launching
 * external commands from plugins (launchbar, menu, wincmd, etc.).
 *
 * Both functions are non-blocking: they return immediately after forking
 * the child process.  Error messages are shown in a GTK dialog on failure.
 *
 * Thread safety: both functions must be called from the GTK main thread only.
 */
#ifndef _RUN_H_
#define _RUN_H_

#include <gtk/gtk.h>

/*
 * run_app -- spawn a shell command asynchronously.
 *
 * Parameters:
 *   cmd - shell command string passed to g_spawn_command_line_async().
 *         The shell is used for word-splitting, globbing, and redirects.
 *         May be NULL (silently returns without error).
 *
 * On failure: shows a GtkMessageDialog with the error and frees GError.
 * On success: child process runs independently; panel does not wait for it.
 *
 * Note: g_spawn_command_line_async runs the command through the shell (/bin/sh).
 * Do not pass untrusted user input — it may be executed as shell code.
 */
void run_app(gchar *cmd);

/*
 * run_app_argv -- spawn a command from an argv array asynchronously.
 *
 * Parameters:
 *   argv - NULL-terminated array of strings.  argv[0] is the executable
 *          name; the PATH is searched (G_SPAWN_SEARCH_PATH).
 *          stdout is redirected to /dev/null (G_SPAWN_STDOUT_TO_DEV_NULL).
 *          Child process is not reaped automatically (G_SPAWN_DO_NOT_REAP_CHILD);
 *          the caller is responsible for calling g_child_watch_add() or
 *          waitpid() to avoid zombie processes.
 *
 * Returns: the GPid of the spawned child on success, or 0 on failure.
 *          On failure: shows a GtkMessageDialog.
 *
 * BUG: G_SPAWN_DO_NOT_REAP_CHILD is set but no child-watch is registered,
 *      so successful spawns will leave zombie processes until the panel exits.
 */
GPid run_app_argv(gchar **argv);

#endif
